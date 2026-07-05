#include "server.h"

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory>
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
#include "connection.h"

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

template <typename Transport>
struct conn_ctx_t
{
  conn_ctx_t(Transport transport, vio::event_loop_t &el, std::shared_ptr<const router_t> r, std::shared_ptr<const logger_t> l, keepalive_options_t o)
    : transport(std::move(transport))
    , loop(el)
    , router(std::move(r))
    , logger(std::move(l))
    , options(o)
  {
  }

  Transport transport;
  vio::event_loop_t &loop;
  std::shared_ptr<const router_t> router;
  std::shared_ptr<const logger_t> logger;
  keepalive_options_t options;
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
void request_flush(std::shared_ptr<conn_ctx_t<Transport>> ctx)
{
  if (ctx->writing || ctx->write_dead)
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
  }(std::move(ctx));
}

template <typename Transport>
void spawn_handler(std::shared_ptr<conn_ctx_t<Transport>> ctx, ready_request_t ready)
{
  ++ctx->inflight;
  [](std::shared_ptr<conn_ctx_t<Transport>> ctx, ready_request_t ready) -> vio::detached_task_t
  {
    ready.request.loop = &ctx->loop;
    std::uint32_t stream_id = ready.stream_id;
    bool head = ready.head;

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
  }(std::move(ctx), std::move(ready));
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
  co_return;
}
} // namespace

vio::task_t<void> serve_connection_h2(vio::tcp_t client, std::shared_ptr<const router_t> router, std::shared_ptr<const logger_t> logger, keepalive_options_t options)
{
  vio::event_loop_t &loop = client.handle->event_loop;
  tcp_transport_t transport{std::move(client), loop, std::nullopt};
  auto ctx = std::make_shared<conn_ctx_t<tcp_transport_t>>(std::move(transport), loop, std::move(router), std::move(logger), options);
  co_await run_connection(ctx);
}

vio::task_t<void> serve_connection_h2_tls(vio::ssl_server_client_t client, std::shared_ptr<const router_t> router, std::shared_ptr<const logger_t> logger, keepalive_options_t options)
{
  vio::event_loop_t &loop = client.handle->event_loop;
  tls_transport_t transport{std::move(client), loop, std::nullopt};
  auto ctx = std::make_shared<conn_ctx_t<tls_transport_t>>(std::move(transport), loop, std::move(router), std::move(logger), options);
  co_await run_connection(ctx);
}
} // namespace prism::detail::http2
