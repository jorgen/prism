#pragma once

#include <string>
#include <string_view>

#include "../../http.h"

namespace prism::detail::websocket
{
inline constexpr std::string_view ws_guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

std::string accept_key(std::string_view sec_websocket_key);

bool is_upgrade_request(const request_t &request);

std::string handshake_response(std::string_view sec_websocket_key);
} // namespace prism::detail::websocket
