#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <vio/task.h>

#include "http.h"

namespace prism
{
struct ws_message_t
{
  std::string data;
  bool is_text = true;
  bool ok = true; // false => the connection has closed; no more messages
};

// A live WebSocket connection handed to an app.ws() handler. The handler is a
// long-lived coroutine that takes the connection by shared_ptr (the dangling-this
// rule) and drives it: receive() pulls inbound messages until the peer closes,
// send_* pushes outbound ones (queued and written by the connection's own writer,
// so send_* from a different coroutine — e.g. a pub/sub push — is safe). Ping/pong
// and the close handshake are handled internally.
class ws_connection_t
{
public:
  virtual ~ws_connection_t() = default;
  ws_connection_t(const ws_connection_t &) = delete;
  ws_connection_t &operator=(const ws_connection_t &) = delete;

  [[nodiscard]] virtual vio::task_t<bool> send_text(std::string_view text) = 0;
  [[nodiscard]] virtual vio::task_t<bool> send_binary(std::span<const std::byte> data) = 0;

  [[nodiscard]] virtual vio::task_t<ws_message_t> receive() = 0;

  virtual void close(std::uint16_t code = 1000, std::string_view reason = "") = 0;

  // The upgraded HTTP request — path params (request().param("token")), headers,
  // loop, and the per-thread factory registry (for per_thread<T> in a ws handler).
  [[nodiscard]] virtual const request_t &request() const = 0;

protected:
  ws_connection_t() = default;
};

using ws_handler_t = std::function<vio::task_t<void>(std::shared_ptr<ws_connection_t>)>;
} // namespace prism
