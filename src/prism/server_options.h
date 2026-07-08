#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace prism
{
enum class protocol_t : std::uint8_t
{
  http1,
  h2c,
};

struct server_options_t
{
  std::chrono::milliseconds idle_timeout = std::chrono::seconds{60};
  std::chrono::milliseconds header_timeout = std::chrono::seconds{30};
  std::chrono::milliseconds body_timeout = std::chrono::seconds{60};
  std::chrono::milliseconds write_timeout = std::chrono::seconds{60};
  std::uint32_t max_requests = 1000;
  std::uint32_t max_connections = 0;

  // Size caps (bytes). 0 disables the relevant cap. 64 KiB of headers clears the
  // vast majority of real-world requests; a buffered body over max_body_bytes is
  // rejected with 413 before dispatch. Streaming routes are not bound by
  // max_body_bytes (the handler controls consumption); max_streaming_body_bytes,
  // when non-zero, caps a streaming reader's not-yet-consumed buffering.
  std::size_t max_header_bytes = std::size_t{64} * 1024;
  std::size_t max_body_bytes = std::size_t{16} * 1024 * 1024;
  std::size_t max_streaming_body_bytes = 0;
  // After a streaming handler returns, prism drains up to this many leftover body
  // bytes to keep the HTTP/1.1 connection alive; beyond it the connection closes.
  std::size_t max_drain_bytes = std::size_t{64} * 1024;

  // Number of event-loop worker threads. 0 (default) = hardware_concurrency, so
  // prism scales across all cores out of the box. 1 forces the single-loop model.
  // Each worker runs its own loop on its own thread, all binding the same port
  // with SO_REUSEPORT (the kernel load-balances accepts). Note: max_connections
  // is per worker; the process ceiling is worker_threads * max_connections.
  std::uint32_t worker_threads = 0;
  // Per-loop graceful-drain budget on shutdown: after the accept loop stops, wait
  // up to this long for in-flight connections to finish before the loop is torn
  // down. 0 = do not wait.
  std::chrono::milliseconds shutdown_timeout = std::chrono::seconds{10};

  protocol_t protocol = protocol_t::http1;
};
} // namespace prism
