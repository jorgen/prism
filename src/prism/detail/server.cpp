#include "prism/detail/server.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <vio/cancellation.h>
#include <vio/error.h>
#include <vio/operation/sleep.h>
#include <vio/operation/tcp.h>
#include <vio/task.h>
#include <vio/unique_buf.h>

#include "prism/detail/http1.h"

namespace prism::detail
{
namespace
{
enum class read_outcome_t : std::uint8_t
{
  data,
  eof,
  timed_out,
  error,
};

void emit(const std::shared_ptr<const logger_t> &logger, log_level_t level, std::string_view message)
{
  if (logger && logger->enabled(level))
  {
    logger->log(level, message);
  }
}

vio::task_t<read_outcome_t> read_with_timeout(vio::event_loop_t &loop, vio::tcp_reader_t &reader, std::chrono::milliseconds timeout, vio::unique_buf_t &out)
{
  if (timeout <= std::chrono::milliseconds::zero())
  {
    auto untimed = co_await reader;
    if (untimed.has_value())
    {
      out = std::move(untimed.value());
      co_return read_outcome_t::data;
    }
    if (untimed.error().code == UV_EOF)
    {
      co_return read_outcome_t::eof;
    }
    co_return read_outcome_t::error;
  }

  vio::cancellation_t token;
  auto watchdog = [](vio::event_loop_t &el, vio::tcp_reader_t &rd, vio::cancellation_t &tok, std::chrono::milliseconds dur) -> vio::task_t<void>
  {
    auto fired = co_await vio::sleep(el, dur, &tok);
    if (fired.has_value() && !tok.is_cancelled())
    {
      rd.cancel();
    }
  }(loop, reader, token, timeout);

  auto read = co_await reader;
  token.cancel();
  co_await std::move(watchdog);

  if (read.has_value())
  {
    out = std::move(read.value());
    co_return read_outcome_t::data;
  }
  if (vio::is_cancelled(read.error()))
  {
    co_return read_outcome_t::timed_out;
  }
  if (read.error().code == UV_EOF)
  {
    co_return read_outcome_t::eof;
  }
  co_return read_outcome_t::error;
}

enum class write_outcome_t : std::uint8_t
{
  ok,
  failed,
  timed_out,
};

vio::task_t<write_outcome_t> write_with_timeout(vio::event_loop_t &loop, vio::tcp_t &client, std::string wire, std::chrono::milliseconds timeout)
{
  if (timeout <= std::chrono::milliseconds::zero())
  {
    auto untimed = co_await vio::write_tcp(client, std::move(wire));
    co_return untimed.has_value() ? write_outcome_t::ok : write_outcome_t::failed;
  }

  vio::cancellation_t token;
  auto fut = vio::write_tcp(client, std::move(wire), &token);

  auto watchdog = [](vio::event_loop_t &el, vio::cancellation_t &write_token, std::chrono::milliseconds dur) -> vio::task_t<void>
  {
    auto fired = co_await vio::sleep(el, dur, &write_token);
    if (fired.has_value() && !write_token.is_cancelled())
    {
      write_token.cancel();
    }
  }(loop, token, timeout);

  auto written = co_await fut;
  token.cancel();
  co_await std::move(watchdog);

  if (written.has_value())
  {
    co_return write_outcome_t::ok;
  }
  if (vio::is_cancelled(written.error()))
  {
    co_return write_outcome_t::timed_out;
  }
  co_return write_outcome_t::failed;
}
} // namespace

vio::task_t<void> serve_connection(vio::tcp_t client, std::shared_ptr<const router_t> router, std::shared_ptr<const logger_t> logger, keepalive_options_t options)
{
  auto reader_or_error = vio::tcp_create_reader(client);
  if (!reader_or_error.has_value())
  {
    co_return;
  }
  auto reader = std::move(reader_or_error.value());
  vio::event_loop_t &loop = client.handle->event_loop;

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
      auto outcome = co_await read_with_timeout(loop, reader, timeout, buffer);
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
          co_await write_with_timeout(loop, client, std::move(wire), options.write_timeout);
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

    response_t response = co_await router->dispatch(std::move(pending.request));
    std::string wire = serialize_response(response, keep_alive, head_request);
    auto outcome = co_await write_with_timeout(loop, client, std::move(wire), options.write_timeout);

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
      co_await serve_connection(std::move(connection), std::move(routes), std::move(log), opts);
      --(*count);
    }(std::move(client.value()), router, logger, options, active);
  }
}
} // namespace prism::detail
