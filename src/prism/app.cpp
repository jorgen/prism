#include "prism/app.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <vio/operation/tcp.h>
#include <vio/operation/tls_server.h>

#include "prism/detail/server.h"
#include "prism/static_files.h"

namespace prism
{
void app_t::static_files(std::string_view url_prefix, std::string root)
{
  std::string pattern(url_prefix);
  if (pattern.empty() || pattern.back() != '/')
  {
    pattern += '/';
  }
  pattern += "{path...}";
  _router.get(pattern, static_file_handler(std::move(root)));
}

vio::task_t<result_t<void>> app_t::listen(vio::event_loop_t &loop, std::string_view host, uint16_t port, vio::cancellation_t *cancel, keepalive_options_t options)
{
  if (!_route_errors.empty())
  {
    for (const auto &route_error : _route_errors)
    {
      _logger->log(log_level_t::error, route_error);
    }
    co_return fail(status_t::internal_server_error, "route configuration error: " + _route_errors.front());
  }

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

vio::task_t<result_t<void>> app_t::listen_tls(vio::event_loop_t &loop, std::string_view host, uint16_t port, vio::ssl_config_t config, vio::cancellation_t *cancel, keepalive_options_t options)
{
  if (!_route_errors.empty())
  {
    for (const auto &route_error : _route_errors)
    {
      _logger->log(log_level_t::error, route_error);
    }
    co_return fail(status_t::internal_server_error, "route configuration error: " + _route_errors.front());
  }

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

  if (config.alpn_protocols.empty())
  {
    config.alpn_protocols = {"h2", "http/1.1"};
  }

  auto tls_server = vio::ssl_server_create(loop, std::move(server.value()), bind_host, config);
  if (!tls_server.has_value())
  {
    _logger->log(log_level_t::error, "ssl_server_create failed: " + tls_server.error().msg);
    co_return fail(status_t::internal_server_error, "ssl_server_create: " + tls_server.error().msg);
  }

  _logger->log(log_level_t::info, "listening on " + bind_host + ":" + std::to_string(port) + " (tls)");
  co_return co_await prism::detail::serve_tls(std::move(tls_server.value()), std::make_shared<const router_t>(_router), _logger, cancel, options);
}

vio::task_t<int> run(vio::event_loop_t &loop, std::string_view host, uint16_t port, std::function<void(app_t &)> configure, keepalive_options_t options)
{
  app_t app;
  if (configure)
  {
    configure(app);
  }
  auto result = co_await app.listen(loop, host, port, nullptr, options);
  co_return result.has_value() ? 0 : 1;
}
} // namespace prism
