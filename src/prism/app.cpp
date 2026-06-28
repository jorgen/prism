#include "prism/app.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <vio/operation/tcp.h>

#include "prism/detail/server.h"

namespace prism
{
vio::task_t<result_t<void>> app_t::listen(vio::event_loop_t &loop, std::string_view host, uint16_t port, vio::cancellation_t *cancel, keepalive_options_t options)
{
  std::string bind_host(host.empty() ? std::string_view("0.0.0.0") : host);

  auto addr = vio::ip4_addr(bind_host, static_cast<int>(port));
  if (!addr.has_value())
  {
    _logger->log(log_level_t::error, "ip4_addr failed: " + addr.error().msg);
    co_return fail(status_t::internal_server_error, "ip4_addr: " + addr.error().msg);
  }

  auto server = vio::tcp_create_server(loop);
  if (!server.has_value())
  {
    _logger->log(log_level_t::error, "tcp_create_server failed: " + server.error().msg);
    co_return fail(status_t::internal_server_error, "tcp_create_server: " + server.error().msg);
  }

  auto bound = vio::tcp_bind(server.value(), reinterpret_cast<const sockaddr *>(&addr.value()));
  if (!bound.has_value())
  {
    _logger->log(log_level_t::error, "tcp_bind failed: " + bound.error().msg);
    co_return fail(status_t::internal_server_error, "tcp_bind: " + bound.error().msg);
  }

  _logger->log(log_level_t::info, "listening on " + bind_host + ":" + std::to_string(port));
  co_return co_await prism::detail::serve(std::move(server.value()), std::make_shared<const router_t>(_router), _logger, cancel, options);
}
} // namespace prism
