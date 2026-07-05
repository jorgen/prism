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

template <typename Transport>
vio::task_t<void> serve_connection_impl(Transport transport, std::shared_ptr<const router_t> router, std::shared_ptr<const logger_t> logger, keepalive_options_t options)
{
  if (!transport.start_reader())
  {
    co_return;
  }
  vio::event_loop_t &loop = transport.loop;

  request_codec_t codec;
  std::uint32_t served = 0;
  bool ended = false;
  for (;;)
  {
    bool request_started = codec.current_in_progress();
    bool body_phase = request_started && codec.current_headers_complete();
    std::chrono::steady_clock::time_point deadline{};
    if (request_started)
    {
      deadline = std::chrono::steady_clock::now() + (body_phase ? options.body_timeout : options.header_timeout);
    }
    while (!codec.has_request() && !ended)
    {
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

    if (!codec.has_request())
    {
      co_return;
    }

    parsed_request_t pending = codec.take_request();
    bool head_request = pending.request.method == method_t::head;
    ++served;
    bool server_close = options.max_requests != 0 && served >= options.max_requests;
    bool keep_alive = pending.keep_alive && !server_close;

    const bool access = logger && logger->enabled(log_level_t::info);
    method_t request_method = pending.request.method;
    std::string request_path;
    std::chrono::steady_clock::time_point started_at{};
    if (access)
    {
      request_path = pending.request.path;
      started_at = std::chrono::steady_clock::now();
    }

    pending.request.loop = &loop;
    response_t response = co_await router->dispatch(std::move(pending.request));
    std::string wire = serialize_response(response, keep_alive, head_request);
    auto outcome = co_await transport.write(std::move(wire), options.write_timeout);

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
        line += std::to_string(static_cast<int>(response.status));
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
