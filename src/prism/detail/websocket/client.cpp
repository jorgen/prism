#include "client.h"

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

#include <vio/crypto.h>
#include <vio/operation/dns.h>
#include <vio/operation/tcp.h>

#include "connection.h"
#include "handshake.h"

namespace prism::detail::websocket
{
namespace
{
bool header_present(const std::vector<header_t> &headers, std::string_view name)
{
  for (const header_t &h : headers)
  {
    if (h.name.size() == name.size())
    {
      bool same = true;
      for (std::size_t i = 0; i < name.size(); ++i)
      {
        char a = h.name[i];
        char b = name[i];
        if (a >= 'A' && a <= 'Z')
        {
          a = static_cast<char>(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z')
        {
          b = static_cast<char>(b - 'A' + 'a');
        }
        if (a != b)
        {
          same = false;
          break;
        }
      }
      if (same)
      {
        return true;
      }
    }
  }
  return false;
}
} // namespace

vio::task_t<std::shared_ptr<ws_connection_t>> connect_client(vio::event_loop_t &loop, std::string host, std::uint16_t port, std::string target, std::vector<header_t> headers, server_options_t options)
{
  vio::address_info_t hints;
  hints.socktype = SOCK_STREAM;
  auto resolved = co_await vio::get_addrinfo(loop, host, hints);
  if (!resolved || resolved->empty())
  {
    co_return nullptr;
  }
  sockaddr *sa = resolved->front().get_sockaddr();
  if (sa == nullptr)
  {
    co_return nullptr;
  }
  if (sa->sa_family == AF_INET)
  {
    reinterpret_cast<sockaddr_in *>(sa)->sin_port = htons(port);
  }
  else if (sa->sa_family == AF_INET6)
  {
    reinterpret_cast<sockaddr_in6 *>(sa)->sin6_port = htons(port);
  }

  auto socket = vio::tcp_create(loop);
  if (!socket)
  {
    co_return nullptr;
  }
  auto connected = co_await vio::tcp_connect(socket.value(), sa);
  if (!connected)
  {
    co_return nullptr;
  }

  tcp_transport_t transport{std::move(socket.value()), loop, std::nullopt};
  if (!transport.start_reader())
  {
    co_return nullptr;
  }

  std::array<std::uint8_t, 16> key_bytes{};
  auto filled = vio::crypto::random_bytes(std::span<std::uint8_t>(key_bytes));
  if (!filled)
  {
    co_return nullptr;
  }
  const std::string key = vio::crypto::base64_encode(std::span<const std::uint8_t>(key_bytes));

  std::string wire = "GET " + target + " HTTP/1.1\r\n";
  if (!header_present(headers, "host"))
  {
    wire += "Host: " + host + "\r\n";
  }
  wire += "Upgrade: websocket\r\nConnection: Upgrade\r\n";
  wire += "Sec-WebSocket-Key: " + key + "\r\nSec-WebSocket-Version: 13\r\n";
  for (const header_t &h : headers)
  {
    wire += h.name + ": " + h.value + "\r\n";
  }
  wire += "\r\n";

  if (co_await transport.write(std::move(wire), options.write_timeout) != write_outcome_t::ok)
  {
    co_return nullptr;
  }

  std::string buffer;
  while (buffer.find("\r\n\r\n") == std::string::npos)
  {
    vio::unique_buf_t chunk;
    auto outcome = co_await transport.read(options.header_timeout, chunk);
    if (outcome != read_outcome_t::data)
    {
      co_return nullptr;
    }
    buffer.append(chunk->base, chunk->len);
    if (buffer.size() > options.max_header_bytes && options.max_header_bytes != 0)
    {
      co_return nullptr;
    }
  }

  const std::size_t header_end = buffer.find("\r\n\r\n") + 4;
  const std::string head = buffer.substr(0, header_end);
  std::string leftover = buffer.substr(header_end);

  if (head.rfind("HTTP/1.1 101", 0) != 0 && head.rfind("HTTP/1.0 101", 0) != 0)
  {
    co_return nullptr;
  }
  if (head.find(accept_key(key)) == std::string::npos)
  {
    co_return nullptr;
  }

  auto state = std::make_shared<ws_state_t<tcp_transport_t>>(std::move(transport), loop, options, request_t{}, /*client_mode=*/true);
  if (!leftover.empty())
  {
    state->reader.feed(leftover.data(), leftover.size());
  }
  auto connection = std::make_shared<ws_connection_impl_t<tcp_transport_t>>(state);

  [](std::shared_ptr<ws_state_t<tcp_transport_t>> pump_state) -> vio::detached_task_t
  {
    co_await read_pump(std::move(pump_state));
  }(state);

  co_return connection;
}
} // namespace prism::detail::websocket
