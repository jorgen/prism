#include "prism/detail/server.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <vio/error.h>
#include <vio/event_loop.h>
#include <vio/operation/tcp.h>
#include <vio/operation/tls_server.h>
#include <vio/task.h>
#include <vio/unique_buf.h>

#include "prism/detail/http1.h"
#include "prism/detail/http2/server.h"
#include "prism/detail/io_timeout.h"

namespace prism::detail
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

// Pull-based inbound body for HTTP/1.1 streaming routes. Holds raw pointers to
// the connection's codec and transport, which live in the serve_connection_impl
// frame; because the handler is co_awaited inline there, the frame outlives the
// reader. Invariant: a streaming handler must not move body_reader into a task
// that outlives dispatch (the pointers would dangle).
template <typename Transport>
class http1_body_reader_t final : public request_body_t
{
public:
  http1_body_reader_t(request_codec_t &codec, Transport &transport, std::chrono::milliseconds body_timeout, std::optional<std::size_t> content_length, bool chunked)
    : _codec(&codec)
    , _transport(&transport)
    , _body_timeout(body_timeout)
    , _content_length(content_length)
    , _chunked(chunked)
    , _remaining(content_length.value_or(0))
  {
    if (_body_timeout > std::chrono::milliseconds::zero())
    {
      _deadline = std::chrono::steady_clock::now() + _body_timeout;
    }
  }

  vio::task_t<std::size_t> read_into(std::span<std::byte> dst) override
  {
    if (dst.empty())
    {
      co_return 0;
    }
    if (_codec->pending_body_size() > 0)
    {
      co_return _codec->take_body_into(dst);
    }
    if (_codec->message_complete() || _status != body_read_status_t::ok)
    {
      finalize_status();
      co_return 0;
    }
    if constexpr (Transport::zero_copy_reads)
    {
      if (_content_length.has_value() && !_chunked)
      {
        if (_remaining == 0 || timed_out_now())
        {
          if (timed_out_now())
          {
            _status = body_read_status_t::timed_out;
          }
          finalize_status();
          co_return 0;
        }
        std::size_t want = std::min(dst.size(), _remaining);
        auto r = co_await _transport->read_into(dst.subspan(0, want), remaining_timeout());
        if (r.outcome == read_outcome_t::data && r.bytes > 0)
        {
          _codec->set_suppress_body_append(true);
          feed_result_t fed = _codec->feed(reinterpret_cast<const char *>(dst.data()), r.bytes);
          _codec->set_suppress_body_append(false);
          if (fed == feed_result_t::error)
          {
            _status = body_read_status_t::aborted;
            co_return 0;
          }
          _remaining -= r.bytes;
          co_return std::size_t{r.bytes};
        }
        apply_read_outcome(r.outcome);
        co_return 0;
      }
    }
    co_await fill_pending();
    if (_codec->pending_body_size() > 0)
    {
      co_return _codec->take_body_into(dst);
    }
    finalize_status();
    co_return 0;
  }

  vio::task_t<body_chunk_t> read_chunk() override
  {
    if (_codec->pending_body_size() == 0 && !_codec->message_complete() && _status == body_read_status_t::ok)
    {
      co_await fill_pending();
    }
    body_chunk_t chunk;
    chunk.data = _codec->take_body_chunk();
    chunk.last = (_codec->message_complete() && _codec->pending_body_size() == 0) || _status != body_read_status_t::ok;
    finalize_status();
    co_return std::move(chunk);
  }

  vio::task_t<std::string> read_all() override
  {
    std::string out;
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
    return _chunked ? std::nullopt : _content_length;
  }
  bool at_end() const override
  {
    return _codec->message_complete() && _codec->pending_body_size() == 0;
  }
  body_read_status_t status() const override
  {
    return _status;
  }

private:
  bool timed_out_now() const
  {
    return _body_timeout > std::chrono::milliseconds::zero() && std::chrono::steady_clock::now() >= _deadline;
  }

  std::chrono::milliseconds remaining_timeout() const
  {
    if (_body_timeout <= std::chrono::milliseconds::zero())
    {
      return std::chrono::milliseconds::zero();
    }
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(_deadline - std::chrono::steady_clock::now());
    return remaining <= std::chrono::milliseconds::zero() ? std::chrono::milliseconds{1} : remaining;
  }

  void apply_read_outcome(read_outcome_t outcome)
  {
    if (outcome == read_outcome_t::eof)
    {
      _codec->finish();
      _status = _codec->message_complete() ? body_read_status_t::end : body_read_status_t::aborted;
    }
    else if (outcome == read_outcome_t::timed_out)
    {
      _status = body_read_status_t::timed_out;
    }
    else if (outcome != read_outcome_t::data)
    {
      _status = body_read_status_t::aborted;
    }
  }

  void finalize_status()
  {
    if (_status == body_read_status_t::ok && _codec->message_complete() && _codec->pending_body_size() == 0)
    {
      _status = body_read_status_t::end;
    }
  }

  vio::task_t<void> fill_pending()
  {
    while (_codec->pending_body_size() == 0 && !_codec->message_complete() && _status == body_read_status_t::ok)
    {
      if (timed_out_now())
      {
        _status = body_read_status_t::timed_out;
        co_return;
      }
      if constexpr (Transport::zero_copy_reads)
      {
        _transport->resume();
      }
      vio::unique_buf_t buffer;
      auto outcome = co_await _transport->read(remaining_timeout(), buffer);
      if constexpr (Transport::zero_copy_reads)
      {
        _transport->pause();
      }
      if (outcome == read_outcome_t::data)
      {
        if (_codec->feed(buffer->base, buffer->len) == feed_result_t::error)
        {
          _status = body_read_status_t::aborted;
          co_return;
        }
        continue;
      }
      apply_read_outcome(outcome);
      co_return;
    }
  }

  request_codec_t *_codec;
  Transport *_transport;
  std::chrono::milliseconds _body_timeout;
  std::chrono::steady_clock::time_point _deadline{};
  std::optional<std::size_t> _content_length;
  bool _chunked = false;
  std::size_t _remaining = 0;
  body_read_status_t _status = body_read_status_t::ok;
};

template <typename Transport>
vio::task_t<write_outcome_t> write_response(Transport &transport, response_t response, bool keep_alive, bool head_request, const keepalive_options_t &options)
{
  if (response.body_stream && !head_request)
  {
    write_outcome_t outcome = co_await transport.write(serialize_streaming_headers(response, keep_alive), options.write_timeout);
    body_source_t source = std::move(response.body_stream);
    while (outcome == write_outcome_t::ok)
    {
      body_chunk_t chunk = co_await source();
      if (!chunk.data.empty())
      {
        outcome = co_await transport.write(serialize_chunk(chunk.data), options.write_timeout);
      }
      if (chunk.last)
      {
        if (outcome == write_outcome_t::ok)
        {
          outcome = co_await transport.write(serialize_last_chunk(), options.write_timeout);
        }
        break;
      }
    }
    co_return std::move(outcome);
  }
  co_return co_await transport.write(serialize_response(response, keep_alive, head_request), options.write_timeout);
}

// Consume any unread remainder of a streaming request body so the connection can
// be kept alive. Returns false (close the connection) if the handler left more
// than max_drain_bytes unread, or on a read/parse error.
template <typename Transport>
vio::task_t<bool> drain_request_body(request_codec_t &codec, Transport &transport, const keepalive_options_t &options)
{
  std::size_t drained = 0;
  for (;;)
  {
    drained += codec.pending_body_size();
    codec.discard_pending_body();
    if (codec.message_complete())
    {
      co_return true;
    }
    if (options.max_drain_bytes != 0 && drained > options.max_drain_bytes)
    {
      co_return false;
    }
    if constexpr (Transport::zero_copy_reads)
    {
      transport.resume();
    }
    vio::unique_buf_t buffer;
    auto outcome = co_await transport.read(options.body_timeout, buffer);
    if constexpr (Transport::zero_copy_reads)
    {
      transport.pause();
    }
    if (outcome != read_outcome_t::data)
    {
      co_return false;
    }
    if (codec.feed(buffer->base, buffer->len) == feed_result_t::error)
    {
      co_return false;
    }
  }
}

template <typename Transport>
vio::task_t<void> serve_connection_impl(Transport transport, std::shared_ptr<const router_t> router, std::shared_ptr<const logger_t> logger, keepalive_options_t options)
{
  if (!transport.start_reader())
  {
    co_return;
  }
  vio::event_loop_t &loop = transport.loop;

  request_codec_t codec;
  codec.set_limits(options.max_header_bytes, options.max_body_bytes, options.max_streaming_body_bytes);
  std::uint32_t served = 0;
  bool ended = false;
  for (;;)
  {
    bool request_started = codec.current_in_progress();
    bool body_phase = request_started && codec.current_headers_complete();
    bool stream_dispatch = false;
    std::chrono::steady_clock::time_point deadline{};
    if (request_started)
    {
      deadline = std::chrono::steady_clock::now() + (body_phase ? options.body_timeout : options.header_timeout);
    }
    while (!codec.has_request() && !ended)
    {
      if (codec.current_headers_complete() && router->resolve(codec.current_method(), codec.current_path()).streaming)
      {
        stream_dispatch = true;
        break;
      }

      std::chrono::milliseconds timeout = options.idle_timeout;
      if (request_started)
      {
        std::chrono::milliseconds budget = body_phase ? options.body_timeout : options.header_timeout;
        if (budget <= std::chrono::milliseconds::zero())
        {
          timeout = std::chrono::milliseconds::zero();
        }
        else
        {
          auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
          if (remaining <= std::chrono::milliseconds::zero())
          {
            emit(logger, log_level_t::warn, body_phase ? "request body timeout; closing connection" : "request header timeout; closing connection");
            co_return;
          }
          timeout = remaining;
        }
      }

      vio::unique_buf_t buffer;
      auto outcome = co_await transport.read(timeout, buffer);
      if (outcome == read_outcome_t::data)
      {
        if (!request_started)
        {
          request_started = true;
          deadline = std::chrono::steady_clock::now() + options.header_timeout;
        }
        if (codec.feed(buffer->base, buffer->len) == feed_result_t::error)
        {
          status_t status = codec.error_status();
          emit(logger, log_level_t::warn, "malformed request -> " + std::to_string(static_cast<int>(status)));
          std::string wire = serialize_response(response_t::text(status, std::string(reason_phrase(status))), false, false);
          co_await transport.write(std::move(wire), options.write_timeout);
          co_return;
        }
        if (!body_phase && codec.current_headers_complete())
        {
          body_phase = true;
          deadline = std::chrono::steady_clock::now() + options.body_timeout;
        }
        continue;
      }
      if (outcome == read_outcome_t::eof)
      {
        codec.finish();
        ended = true;
        break;
      }
      if (outcome == read_outcome_t::timed_out)
      {
        if (request_started)
        {
          emit(logger, log_level_t::warn, body_phase ? "request body timeout; closing connection" : "request header timeout; closing connection");
        }
        else
        {
          emit(logger, log_level_t::debug, "idle connection closed");
        }
      }
      else
      {
        emit(logger, log_level_t::debug, "read error; closing connection");
      }
      co_return;
    }

    if (!codec.has_request() && !stream_dispatch)
    {
      co_return;
    }

    ++served;
    bool server_close = options.max_requests != 0 && served >= options.max_requests;

    request_t request;
    bool keep_alive = false;
    bool streaming = false;
    if (stream_dispatch)
    {
      streaming = true;
      keep_alive = codec.current_keep_alive() && !server_close;
      std::optional<std::size_t> content_length = codec.current_content_length();
      bool chunked = codec.current_is_chunked();
      codec.begin_streaming_body();
      request = codec.take_header_request();
      request.body_reader = std::make_shared<http1_body_reader_t<Transport>>(codec, transport, options.body_timeout, content_length, chunked);
    }
    else
    {
      parsed_request_t pending = codec.take_request();
      keep_alive = pending.keep_alive && !server_close;
      auto match = router->resolve(pending.request.method, pending.request.path);
      request = std::move(pending.request);
      if (match.streaming)
      {
        streaming = true;
        std::optional<std::size_t> content_length = codec.current_content_length();
        bool chunked = codec.current_is_chunked();
        std::string body = std::move(request.body);
        request.body.clear();
        codec.preload_complete_body(std::move(body));
        request.body_reader = std::make_shared<http1_body_reader_t<Transport>>(codec, transport, options.body_timeout, content_length, chunked);
      }
    }

    bool head_request = request.method == method_t::head;
    const bool access = logger && logger->enabled(log_level_t::info);
    method_t request_method = request.method;
    std::string request_path;
    std::chrono::steady_clock::time_point started_at{};
    if (access)
    {
      request_path = request.path;
      started_at = std::chrono::steady_clock::now();
    }

    request.loop = &loop;
    response_t response = co_await router->dispatch(std::move(request));

    if (streaming && !co_await drain_request_body(codec, transport, options))
    {
      keep_alive = false;
    }

    status_t response_status = response.status;
    write_outcome_t outcome = co_await write_response(transport, std::move(response), keep_alive, head_request, options);

    if (outcome == write_outcome_t::ok)
    {
      if (access)
      {
        auto micros = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started_at).count();
        std::string line;
        line.reserve(64);
        line += method_name(request_method);
        line += ' ';
        line += request_path;
        line += ' ';
        line += std::to_string(static_cast<int>(response_status));
        line += ' ';
        line += std::to_string(micros);
        line += "us";
        logger->log(log_level_t::info, line);
      }
    }
    else if (outcome == write_outcome_t::timed_out)
    {
      emit(logger, log_level_t::warn, "response write timeout; closing connection");
    }
    else
    {
      emit(logger, log_level_t::warn, "response write failed; closing connection");
    }

    if (outcome != write_outcome_t::ok || !keep_alive)
    {
      co_return;
    }
  }
}

vio::task_t<void> serve_connection_tls(vio::ssl_server_client_t client, std::shared_ptr<const router_t> router, std::shared_ptr<const logger_t> logger, keepalive_options_t options)
{
  vio::event_loop_t &loop = client.handle->event_loop;
  co_await serve_connection_impl(tls_transport_t{std::move(client), loop, std::nullopt}, std::move(router), std::move(logger), options);
}
} // namespace

vio::task_t<void> serve_connection(vio::tcp_t client, std::shared_ptr<const router_t> router, std::shared_ptr<const logger_t> logger, keepalive_options_t options)
{
  vio::event_loop_t &loop = client.handle->event_loop;
  co_await serve_connection_impl(tcp_transport_t{std::move(client), loop, std::nullopt}, std::move(router), std::move(logger), options);
}

vio::task_t<result_t<void>> serve(vio::tcp_server_t server, std::shared_ptr<const router_t> router, std::shared_ptr<const logger_t> logger, vio::cancellation_t *cancel, keepalive_options_t options)
{
  auto active = std::make_shared<std::size_t>(0);
  for (;;)
  {
    auto listen_result = co_await vio::tcp_listen(server, 128, cancel);
    if (!listen_result.has_value())
    {
      if (cancel != nullptr && vio::is_cancelled(listen_result.error()))
      {
        emit(logger, log_level_t::info, "server stopped");
        co_return result_t<void>{};
      }
      emit(logger, log_level_t::error, "accept loop stopped: " + listen_result.error().msg);
      co_return fail(status_t::internal_server_error, "tcp_listen: " + listen_result.error().msg);
    }

    auto client = vio::tcp_accept(server);
    server.tcp.handle->listen.done = false;
    if (!client.has_value())
    {
      continue;
    }

    if (options.max_connections != 0 && *active >= options.max_connections)
    {
      emit(logger, log_level_t::warn, "connection rejected: max_connections reached");
      continue;
    }

    ++(*active);
    emit(logger, log_level_t::debug, "connection accepted");
    [](vio::tcp_t connection, std::shared_ptr<const router_t> routes, std::shared_ptr<const logger_t> log, keepalive_options_t opts, std::shared_ptr<std::size_t> count) -> vio::detached_task_t
    {
      if (opts.protocol == protocol_t::h2c)
      {
        co_await http2::serve_connection_h2(std::move(connection), std::move(routes), std::move(log), opts);
      }
      else
      {
        co_await serve_connection(std::move(connection), std::move(routes), std::move(log), opts);
      }
      --(*count);
    }(std::move(client.value()), router, logger, options, active);
  }
}

vio::task_t<result_t<void>> serve_tls(vio::ssl_server_t server, std::shared_ptr<const router_t> router, std::shared_ptr<const logger_t> logger, vio::cancellation_t *cancel, keepalive_options_t options)
{
  auto active = std::make_shared<std::size_t>(0);
  for (;;)
  {
    auto listen_result = co_await vio::ssl_server_listen(server, 128, cancel);
    if (!listen_result.has_value())
    {
      if (cancel != nullptr && vio::is_cancelled(listen_result.error()))
      {
        emit(logger, log_level_t::info, "server stopped");
        co_return result_t<void>{};
      }
      emit(logger, log_level_t::error, "accept loop stopped: " + listen_result.error().msg);
      co_return fail(status_t::internal_server_error, "ssl_server_listen: " + listen_result.error().msg);
    }

    auto client = vio::ssl_server_accept(server);
    server.handle->tcp.tcp.handle->listen.done = false;
    if (!client.has_value())
    {
      continue;
    }

    if (options.max_connections != 0 && *active >= options.max_connections)
    {
      emit(logger, log_level_t::warn, "connection rejected: max_connections reached");
      continue;
    }

    ++(*active);
    emit(logger, log_level_t::debug, "tls connection accepted");
    [](vio::ssl_server_client_t connection, std::shared_ptr<const router_t> routes, std::shared_ptr<const logger_t> log, keepalive_options_t opts, std::shared_ptr<std::size_t> count) -> vio::detached_task_t
    {
      auto handshake = co_await vio::ssl_server_client_handshake(connection);
      if (handshake.has_value())
      {
        auto negotiated = vio::ssl_server_client_alpn_selected(connection);
        if (negotiated.has_value() && *negotiated == "h2")
        {
          co_await http2::serve_connection_h2_tls(std::move(connection), std::move(routes), std::move(log), opts);
        }
        else
        {
          co_await serve_connection_tls(std::move(connection), std::move(routes), std::move(log), opts);
        }
      }
      --(*count);
    }(std::move(client.value()), router, logger, options, active);
  }
}
} // namespace prism::detail
