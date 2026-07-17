#include "handshake.h"

#include <array>
#include <cctype>
#include <cstdint>
#include <span>

#include <vio/crypto.h>

namespace prism::detail::websocket
{
namespace
{
bool iequals(std::string_view a, std::string_view b)
{
  if (a.size() != b.size())
  {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i)
  {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
    {
      return false;
    }
  }
  return true;
}

bool header_has_token(const std::string *value, std::string_view token)
{
  if (value == nullptr)
  {
    return false;
  }
  std::string_view text = *value;
  std::size_t pos = 0;
  while (pos < text.size())
  {
    std::size_t comma = text.find(',', pos);
    std::string_view part = text.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos);
    std::size_t begin = part.find_first_not_of(" \t");
    if (begin != std::string_view::npos)
    {
      std::size_t end = part.find_last_not_of(" \t");
      if (iequals(part.substr(begin, end - begin + 1), token))
      {
        return true;
      }
    }
    if (comma == std::string_view::npos)
    {
      break;
    }
    pos = comma + 1;
  }
  return false;
}
} // namespace

std::string accept_key(std::string_view sec_websocket_key)
{
  std::string combined;
  combined.reserve(sec_websocket_key.size() + ws_guid.size());
  combined.append(sec_websocket_key);
  combined.append(ws_guid);
  vio::crypto::sha1_digest_t digest = vio::crypto::sha1(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(combined.data()), combined.size()));
  return vio::crypto::base64_encode(std::span<const std::uint8_t>(digest.data(), digest.size()));
}

bool is_upgrade_request(const request_t &request)
{
  const std::string *upgrade = request.headers.find("Upgrade");
  if (upgrade == nullptr || !iequals(*upgrade, "websocket"))
  {
    return false;
  }
  if (!header_has_token(request.headers.find("Connection"), "upgrade"))
  {
    return false;
  }
  const std::string *version = request.headers.find("Sec-WebSocket-Version");
  if (version == nullptr || *version != "13")
  {
    return false;
  }
  const std::string *key = request.headers.find("Sec-WebSocket-Key");
  return key != nullptr && !key->empty();
}

std::string handshake_response(std::string_view sec_websocket_key)
{
  std::string response = "HTTP/1.1 101 Switching Protocols\r\n";
  response += "Upgrade: websocket\r\n";
  response += "Connection: Upgrade\r\n";
  response += "Sec-WebSocket-Accept: ";
  response += accept_key(sec_websocket_key);
  response += "\r\n\r\n";
  return response;
}
} // namespace prism::detail::websocket
