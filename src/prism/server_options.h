#pragma once

#include <chrono>
#include <cstdint>

namespace prism
{
enum class protocol_t : std::uint8_t
{
  http1,
  h2c,
};

struct keepalive_options_t
{
  std::chrono::milliseconds idle_timeout = std::chrono::seconds{60};
  std::chrono::milliseconds header_timeout = std::chrono::seconds{30};
  std::chrono::milliseconds body_timeout = std::chrono::seconds{60};
  std::chrono::milliseconds write_timeout = std::chrono::seconds{60};
  std::uint32_t max_requests = 1000;
  std::uint32_t max_connections = 0;
  protocol_t protocol = protocol_t::http1;
};
} // namespace prism
