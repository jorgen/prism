#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <vio/event_loop.h>
#include <vio/task.h>

#include "../../http.h"
#include "../../server_options.h"
#include "../../websocket.h"

namespace prism::detail::websocket
{
// Open a client WebSocket to host:port over plaintext TCP, performing the RFC
// 6455 handshake (with a fresh Sec-WebSocket-Key) and forwarding the given extra
// headers. Returns a connected client-mode ws_connection_t, or nullptr on any
// failure (DNS, connect, handshake). Used by the reverse proxy's WS relay to dial
// an internal backend.
vio::task_t<std::shared_ptr<ws_connection_t>> connect_client(vio::event_loop_t &loop, std::string host, std::uint16_t port, std::string target, std::vector<header_t> headers, server_options_t options);
} // namespace prism::detail::websocket
