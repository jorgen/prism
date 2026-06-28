#pragma once

#include <chrono>
#include <cstdint>

namespace prism
{
struct keepalive_options_t
{
  std::chrono::milliseconds idle_timeout = std::chrono::seconds{60};
  std::chrono::milliseconds header_timeout = std::chrono::seconds{30};
  std::chrono::milliseconds body_timeout = std::chrono::seconds{60};
  std::chrono::milliseconds write_timeout = std::chrono::seconds{60};
  std::uint32_t max_requests = 1000;
  std::uint32_t max_connections = 0;
};
} // namespace prism
