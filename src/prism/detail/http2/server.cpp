#include "server.h"

#include <algorithm>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vio/event_loop.h>
#include <vio/operation/tcp.h>
#include <vio/operation/tls_server.h>
#include <vio/task.h>
#include <vio/unique_buf.h>

#include "../io_timeout.h"
#include "../websocket/connection.h"
#include "connection.h"
#include "../../router.h"
#include "../../websocket.h"

namespace prism::detail::http2
{
namespace
{
void emit(const std::shared_ptr<const logger_t> &logger, log_level_t level, std::string_view message)
{
  if (logger && logger->enabled(level))
  {
    logger->log(level, message);
  }
}

inline h2_settings_t h2_settings_from(const server_options_t &options, const router_t &router)
{
  h2_settings_t settings;
  if (options.max_body_bytes != 0)
  {
    settings.max_body_bytes = options.max_body_bytes;
  }
  if (options.max_header_bytes != 0)
  {
    settings.max_header_list_size = static_cast<std::uint32_t>(options.max_header_bytes);
  }
  settings.enable_connect_protocol = router.has_websocket_routes();
  return settings;
}

template <typename Transport>
struct conn_ctx_t
{
  conn_ctx_t(Transport transport_arg, vio::event_loop_t &el, std::shared_ptr<const router_t> r, std::shared_ptr<const logger_t> l, server_options_t o, std::string client_ip_arg)
    : transport(std::move(transport_arg))
    , loop(el)
    , router(std::move(r))
    , logger(std::move(l))
    , options(o)
    , client_ip(std::move(client_ip_arg))
    , conn(h2_settings_from(o, *router), [routes = router](method_t method, std::string_view path) { return routes->is_streaming(method, path); }, [routes = router](std::string_view path) { return routes->resolve(method_t::get, path).websocket; })
  {
  }

  Transport transport;
  vio::event_loop_t &loop;
  std::shared_ptr<const router_t> router;
  std::shared_ptr<const logger_t> logger;
  server_options_t options;
  std::string client_ip;
  connection_t conn;
  bool writing = false;
  bool write_dead = false;
  bool closing = false;
  std::size_t inflight = 0;
  std::vector<std::coroutine_handle<>> flow_waiters;
};

template <typename Transport>
void wake_flow(conn_ctx_t<Transport> &ctx)
{
  std::vector<std::coroutine_handle<>> waiters;
  waiters.swap(ctx.flow_waiters);
  for (std::coroutine_handle<> handle : waiters)
  {
    handle.resume();
  }
}

template <typename Transport>
struct flow_gate_t
{
  std::shared_ptr<conn_ctx_t<Transport>> ctx;
  [[nodiscard]] bool await_ready() const noexcept
  {
    return ctx->write_dead || ctx->closing;
  }
  void await_suspend(std::coroutine_handle<> handle) const
  {
    ctx->flow_waiters.push_back(handle);
  }
  void await_resume() const noexcept
  {
  }
};

template <typename Transport>
void request_flush(std::shared_ptr<conn_ctx_t<Transport>> state)
{
  if (state->writing || state->write_dead)
  {
    return;
  }
  [](std::shared_ptr<conn_ctx_t<Transport>> ctx) -> vio::detached_task_t
  {
    ctx->writing = true;
    for (;;)
    {
      std::string out = ctx->conn.take_output();
      if (out.empty())
      {
        break;
      }
      auto outcome = co_await ctx->transport.write(std::move(out), ctx->options.write_timeout);
      if (outcome != write_outcome_t::ok)
      {
        ctx->write_dead = true;
        break;
      }
    }
    ctx->writing = false;
    wake_flow(*ctx);
  }(std::move(state));
}

// Awaitable that parks the handler until the stream has a queued DATA chunk, has
// ended, or was aborted (mirrors flow_gate_t for the inbound direction). The
// driver resumes the parked handle via collect_inbound_ready after receive().
template <typename Transport>
struct inbound_gate_t
{
  std::shared_ptr<conn_ctx_t<Transport>> ctx;
  std::uint32_t stream_id;
  [[nodiscard]] bool await_ready() const noexcept
  {
    return ctx->write_dead || ctx->closing || ctx->conn.inbound_has_chunk(stream_id) || ctx->conn.inbound_ended(stream_id) || ctx->conn.inbound_aborted(stream_id);
  }
  void await_suspend(std::coroutine_handle<> handle) const
  {
    ctx->conn.set_inbound_waiter(stream_id, handle);
  }
  void await_resume() const noexcept
  {
  }
};

template <typename Transport>
class request_body_h2_t final : public request_body_t
{
public:
  request_body_h2_t(std::shared_ptr<conn_ctx_t<Transport>> ctx, std::uint32_t stream_id)
    : _ctx(std::move(ctx))
    , _stream_id(stream_id)
    , _length(_ctx->conn.inbound_length(stream_id))
  {
  }

  vio::task_t<body_chunk_t> read_chunk() override
  {
    for (;;)
    {
      std::string data;
      bool last = false;
      if (_ctx->conn.take_inbound_chunk(_stream_id, data, last))
      {
        _ctx->conn.inbound_consume(_stream_id, data.size());
        request_flush(_ctx);
        if (last)
        {
          _status = body_read_status_t::end;
        }
        co_return body_chunk_t{std::move(data), last};
      }
      if (_ctx->conn.inbound_aborted(_stream_id) || _ctx->write_dead || _ctx->closing)
      {
        _status = body_read_status_t::aborted;
        co_return body_chunk_t{{}, true};
      }
      if (_ctx->conn.inbound_ended(_stream_id))
      {
        _status = body_read_status_t::end;
        co_return body_chunk_t{{}, true};
      }
      co_await inbound_gate_t<Transport>{_ctx, _stream_id};
    }
  }

  vio::task_t<std::size_t> read_into(std::span<std::byte> dst) override
  {
    if (dst.empty())
    {
      co_return 0;
    }
    while (_pending_offset == _pending.size())
    {
      body_chunk_t chunk = co_await read_chunk();
      _pending = std::move(chunk.data);
      _pending_offset = 0;
      if (_pending.empty() && chunk.last)
      {
        co_return 0;
      }
    }
    std::size_t n = std::min(dst.size(), _pending.size() - _pending_offset);
    std::memcpy(dst.data(), _pending.data() + _pending_offset, n);
    _pending_offset += n;
    co_return std::size_t{n};
  }

  vio::task_t<std::string> read_all() override
  {
    std::string out;
    if (_pending_offset < _pending.size())
    {
      out.append(_pending, _pending_offset);
      _pending.clear();
      _pending_offset = 0;
    }
    for (;;)
    {
      body_chunk_t chunk = co_await read_chunk();
      out += chunk.data;
      if (chunk.last)
      {
        break;
      }
    }
    co_return std::move(out);
  }

  std::optional<std::size_t> length() const override
  {
    return _length;
  }
  bool at_end() const override
  {
    return _pending_offset == _pending.size() && _status == body_read_status_t::end;
  }
  body_read_status_t status() const override
  {
    return _status;
  }

private:
  std::shared_ptr<conn_ctx_t<Transport>> _ctx;
  std::uint32_t _stream_id;
  std::optional<std::size_t> _length;
  std::string _pending;
  std::size_t _pending_offset = 0;
  body_read_status_t _status = body_read_status_t::ok;
};

template <typename Transport>
struct h2_stream_transport_t
{
  std::shared_ptr<conn_ctx_t<Transport>> ctx;
  std::uint32_t stream_id = 0;
  bool cancelled = false;
  bool finished = false;

  vio::task_t<read_outcome_t> read(std::chrono::milliseconds /*timeout*/, vio::unique_buf_t &out)
  {
    for (;;)
    {
      if (cancelled)
      {
        co_return read_outcome_t::error;
      }
      std::string data;
      bool last = false;
      if (ctx->conn.take_inbound_chunk(stream_id, data, last))
      {
        ctx->conn.inbound_consume(stream_id, data.size());
        request_flush(ctx);
        if (!data.empty())
        {
          char *base = new char[data.size()];
          std::memcpy(base, data.data(), data.size());
          uv_buf_t buf;
          buf.base = base;
          buf.len = static_cast<decltype(buf.len)>(data.size());
          out = vio::unique_buf_t{buf, vio::default_dealloc, nullptr};
          co_return read_outcome_t::data;
        }
        if (last)
        {
          co_return read_outcome_t::eof;
        }
        continue;
      }
      if (ctx->conn.inbound_aborted(stream_id) || ctx->write_dead || ctx->closing)
      {
        co_return read_outcome_t::error;
      }
      if (ctx->conn.inbound_ended(stream_id))
      {
        co_return read_outcome_t::eof;
      }
      co_await inbound_gate_t<Transport>{ctx, stream_id};
      if (cancelled)
      {
        co_return read_outcome_t::error;
      }
    }
  }

  vio::task_t<write_outcome_t> write(std::string wire, std::chrono::milliseconds timeout)
  {
    if (ctx->write_dead || ctx->closing || ctx->conn.failed())
    {
      co_return write_outcome_t::failed;
    }
    ctx->conn.push_stream_data(stream_id, wire, false);
    request_flush(ctx);
    if (ctx->conn.stream_send_pending(stream_id) > 0 && timeout > std::chrono::milliseconds::zero())
    {
      vio::cancellation_t token;
      auto watchdog = [](std::shared_ptr<conn_ctx_t<Transport>> c, vio::cancellation_t &tok, std::chrono::milliseconds dur) -> vio::task_t<void>
      {
        auto fired = co_await vio::sleep(c->loop, dur, &tok);
        if (fired.has_value() && !tok.is_cancelled())
        {
          c->write_dead = true;
          wake_flow(*c);
        }
      }(ctx, token, timeout);
      while (ctx->conn.stream_send_pending(stream_id) > 0 && !ctx->write_dead && !ctx->closing)
      {
        co_await flow_gate_t<Transport>{ctx};
      }
      token.cancel();
      co_await std::move(watchdog);
    }
    co_return (ctx->write_dead || ctx->closing) ? write_outcome_t::failed : write_outcome_t::ok;
  }

  void cancel_read()
  {
    cancelled = true;
    ctx->conn.wake_inbound(stream_id);
  }

  void finish()
  {
    if (finished)
    {
      return;
    }
    finished = true;
    if (!ctx->write_dead && !ctx->closing)
    {
      ctx->conn.push_stream_data(stream_id, {}, true);
      request_flush(ctx);
    }
  }
};

template <typename Transport>
void spawn_handler(std::shared_ptr<conn_ctx_t<Transport>> state, ready_request_t request)
{
  ++state->inflight;
  [](std::shared_ptr<conn_ctx_t<Transport>> ctx, ready_request_t ready) -> vio::detached_task_t
  {
    ready.request.loop = &ctx->loop;
    ready.request.client_ip = ctx->client_ip;
    std::uint32_t stream_id = ready.stream_id;
    bool head = ready.head;
    bool streaming = ready.streaming;

    if (ready.websocket)
    {
      ws_handler_t handler = ctx->router->match_websocket(ready.request);
      if (handler && !ctx->conn.inbound_aborted(stream_id) && !ctx->closing && !ctx->write_dead)
      {
        ctx->conn.begin_streaming_response(stream_id, response_t{}, false);
        request_flush(ctx);
        h2_stream_transport_t<Transport> transport{ctx, stream_id, false, false};
        co_await websocket::run_websocket<h2_stream_transport_t<Transport>>(std::move(transport), std::move(ready.request), std::move(handler), ctx->loop, ctx->options);
      }
      else
      {
        ctx->conn.reset_stream(stream_id, error_code_t::refused_stream);
        request_flush(ctx);
      }
      --ctx->inflight;
      request_flush(std::move(ctx));
      co_return;
    }

    if (streaming)
    {
      ready.request.body_reader = std::make_shared<request_body_h2_t<Transport>>(ctx, stream_id);
    }

    const bool access = ctx->logger && ctx->logger->enabled(log_level_t::info);
    method_t method = ready.request.method;
    std::string path;
    std::chrono::steady_clock::time_point started_at{};
    if (access)
    {
      path = ready.request.path;
      started_at = std::chrono::steady_clock::now();
    }

    response_t response = co_await ctx->router->dispatch(std::move(ready.request));
    status_t status = response.status;
    if (response.body_stream)
    {
      ctx->conn.begin_streaming_response(stream_id, response, head);
      request_flush(ctx);
      body_source_t source = std::move(response.body_stream);
      for (;;)
      {
        body_chunk_t chunk = co_await source();
        if (ctx->conn.failed() || ctx->write_dead || ctx->closing)
        {
          break;
        }
        ctx->conn.push_stream_data(stream_id, chunk.data, chunk.last);
        request_flush(ctx);
        if (chunk.last)
        {
          break;
        }
        while (ctx->conn.stream_send_pending(stream_id) > 0 && !ctx->write_dead && !ctx->closing)
        {
          co_await flow_gate_t<Transport>{ctx};
        }
      }
    }
    else
    {
      ctx->conn.submit_response(stream_id, std::move(response), head);
    }

    // If the handler didn't consume the whole request body, RST the stream so
    // the client stops uploading (legal after a complete response).
    if (streaming && !ctx->conn.inbound_ended(stream_id) && !ctx->conn.inbound_aborted(stream_id))
    {
      ctx->conn.abort_inbound(stream_id, error_code_t::no_error);
      request_flush(ctx);
    }

    --ctx->inflight;

    if (access)
    {
      auto micros = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started_at).count();
      std::string line;
      line.reserve(64);
      line += method_name(method);
      line += ' ';
      line += path;
      line += ' ';
      line += std::to_string(static_cast<int>(status));
      line += ' ';
      line += std::to_string(micros);
      line += "us";
      ctx->logger->log(log_level_t::info, line);
    }

    request_flush(std::move(ctx));
  }(std::move(state), std::move(request));
}

template <typename Transport>
vio::task_t<void> run_connection(std::shared_ptr<conn_ctx_t<Transport>> ctx)
{
  if (!ctx->transport.start_reader())
  {
    co_return;
  }

  ctx->conn.start();
  request_flush(ctx);

  for (;;)
  {
    std::chrono::milliseconds timeout = std::chrono::milliseconds::zero();
    if (ctx->inflight == 0)
    {
      timeout = ctx->conn.active_streams() > 0 ? ctx->options.header_timeout : ctx->options.idle_timeout;
    }

    vio::unique_buf_t buffer;
    auto outcome = co_await ctx->transport.read(timeout, buffer);
    if (outcome != read_outcome_t::data)
    {
      if (outcome == read_outcome_t::timed_out)
      {
        emit(ctx->logger, log_level_t::debug, "http2 connection idle/stalled; closing");
      }
      break;
    }

    std::vector<ready_request_t> ready;
    bool ok = ctx->conn.receive(std::string_view(buffer->base, buffer->len), ready);
    for (ready_request_t &request : ready)
    {
      spawn_handler(ctx, std::move(request));
    }
    // Resume any streaming-body readers whose stream got fresh DATA / END_STREAM
    // / RST this round (mirrors wake_flow; resumed outside receive()'s frame loop).
    {
      std::vector<std::coroutine_handle<>> woken;
      ctx->conn.collect_inbound_ready(woken);
      for (std::coroutine_handle<> handle : woken)
      {
        handle.resume();
      }
    }
    if (ctx->options.max_requests != 0 && ctx->conn.streams_opened() >= ctx->options.max_requests)
    {
      ctx->conn.begin_goaway();
    }
    request_flush(ctx);

    if (!ok || ctx->conn.failed() || ctx->write_dead)
    {
      emit(ctx->logger, log_level_t::warn, "http2 connection error; closing");
      break;
    }
    if (ctx->conn.wants_close() && ctx->inflight == 0)
    {
      break;
    }
  }
  ctx->closing = true;
  wake_flow(*ctx);
  // Unblock any streaming-body reader still parked so its detached handler can
  // unwind (and release its ctx co-ownership).
  ctx->conn.fail_all_inbound();
  std::vector<std::coroutine_handle<>> woken;
  ctx->conn.collect_inbound_ready(woken);
  for (std::coroutine_handle<> handle : woken)
  {
    handle.resume();
  }
  co_return;
}
} // namespace

vio::task_t<void> serve_connection_h2(vio::tcp_t client, std::shared_ptr<const router_t> router, std::shared_ptr<const logger_t> logger, server_options_t options)
{
  vio::event_loop_t &loop = client.handle->event_loop;
  std::string client_ip = vio::peer_ip(client.get_tcp());
  tcp_transport_t transport{std::move(client), loop, std::nullopt};
  auto ctx = std::make_shared<conn_ctx_t<tcp_transport_t>>(std::move(transport), loop, std::move(router), std::move(logger), options, std::move(client_ip));
  co_await run_connection(ctx);
}

vio::task_t<void> serve_connection_h2_tls(vio::ssl_server_client_t client, std::shared_ptr<const router_t> router, std::shared_ptr<const logger_t> logger, server_options_t options)
{
  vio::event_loop_t &loop = client.handle->event_loop;
  std::string client_ip = vio::ssl_server_client_peer_ip(client);
  tls_transport_t transport{std::move(client), loop, std::nullopt};
  auto ctx = std::make_shared<conn_ctx_t<tls_transport_t>>(std::move(transport), loop, std::move(router), std::move(logger), options, std::move(client_ip));
  co_await run_connection(ctx);
}
} // namespace prism::detail::http2
