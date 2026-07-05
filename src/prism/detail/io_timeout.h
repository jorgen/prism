#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include <vio/cancellation.h>
#include <vio/error.h>
#include <vio/event_loop.h>
#include <vio/operation/sleep.h>
#include <vio/operation/tcp.h>
#include <vio/operation/tls_server.h>
#include <vio/task.h>
#include <vio/unique_buf.h>

namespace prism::detail
{
enum class read_outcome_t : std::uint8_t
{
  data,
  eof,
  timed_out,
  error,
};

enum class write_outcome_t : std::uint8_t
{
  ok,
  failed,
  timed_out,
};

template <typename Reader>
vio::task_t<read_outcome_t> read_with_timeout(vio::event_loop_t &loop, Reader &reader, std::chrono::milliseconds timeout, vio::unique_buf_t &out)
{
  if (timeout <= std::chrono::milliseconds::zero())
  {
    auto untimed = co_await reader;
    if (untimed.has_value())
    {
      out = std::move(untimed.value());
      co_return read_outcome_t::data;
    }
    co_return untimed.error().code == UV_EOF ? read_outcome_t::eof : read_outcome_t::error;
  }

  vio::cancellation_t token;
  auto watchdog = [](vio::event_loop_t &el, Reader &rd, vio::cancellation_t &tok, std::chrono::milliseconds dur) -> vio::task_t<void>
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
  co_return read.error().code == UV_EOF ? read_outcome_t::eof : read_outcome_t::error;
}

inline vio::task_t<write_outcome_t> write_tcp_with_timeout(vio::event_loop_t &loop, vio::tcp_t &client, std::string wire, std::chrono::milliseconds timeout)
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

inline vio::task_t<write_outcome_t> write_tls_bytes(vio::ssl_server_client_t &client, std::string wire)
{
  uv_buf_t buffer;
  buffer.base = wire.data();
  buffer.len = static_cast<decltype(buffer.len)>(wire.size());
  auto written = co_await vio::ssl_server_client_write(client, buffer);
  co_return written.has_value() ? write_outcome_t::ok : write_outcome_t::failed;
}

struct tcp_transport_t
{
  vio::tcp_t socket;
  vio::event_loop_t &loop;
  std::optional<vio::tcp_reader_t> reader;

  bool start_reader()
  {
    auto created = vio::tcp_create_reader(socket);
    if (!created.has_value())
    {
      return false;
    }
    reader.emplace(std::move(created.value()));
    return true;
  }

  vio::task_t<read_outcome_t> read(std::chrono::milliseconds timeout, vio::unique_buf_t &out)
  {
    return read_with_timeout(loop, *reader, timeout, out);
  }

  vio::task_t<write_outcome_t> write(std::string wire, std::chrono::milliseconds timeout)
  {
    return write_tcp_with_timeout(loop, socket, std::move(wire), timeout);
  }
};

struct tls_transport_t
{
  vio::ssl_server_client_t socket;
  vio::event_loop_t &loop;
  std::optional<vio::tls_server_client_reader_t> reader;

  bool start_reader()
  {
    auto created = vio::ssl_server_client_create_reader(socket);
    if (!created.has_value())
    {
      return false;
    }
    reader.emplace(std::move(created.value()));
    return true;
  }

  vio::task_t<read_outcome_t> read(std::chrono::milliseconds timeout, vio::unique_buf_t &out)
  {
    return read_with_timeout(loop, *reader, timeout, out);
  }

  vio::task_t<write_outcome_t> write(std::string wire, std::chrono::milliseconds timeout)
  {
    (void)timeout;
    return write_tls_bytes(socket, std::move(wire));
  }
};
} // namespace prism::detail
