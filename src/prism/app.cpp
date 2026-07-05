#include "prism/app.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <vio/operation/tcp.h>
#include <vio/operation/tcp_server.h>
#include <vio/operation/tls_server.h>

#include "prism/detail/server.h"
#include "prism/static_files.h"

namespace prism
{
namespace
{
result_t<vio::tcp_server_t> bind_one(vio::event_loop_t &loop, const std::string &host, bool ipv6, uint16_t port)
{
  auto server = vio::tcp_create_server(loop);
  if (!server.has_value())
  {
    return fail(status_t::internal_server_error, "tcp_create_server: " + server.error().msg);
  }
  if (ipv6)
  {
    auto addr = vio::ip6_addr(host, static_cast<int>(port));
    if (!addr.has_value())
    {
      return fail(status_t::internal_server_error, "ip6_addr: " + addr.error().msg);
    }
    auto bound = vio::tcp_bind(server.value(), reinterpret_cast<const sockaddr *>(&addr.value()));
    if (!bound.has_value())
    {
      return fail(status_t::internal_server_error, "tcp_bind: " + bound.error().msg);
    }
  }
  else
  {
    auto addr = vio::ip4_addr(host, static_cast<int>(port));
    if (!addr.has_value())
    {
      return fail(status_t::internal_server_error, "ip4_addr: " + addr.error().msg);
    }
    auto bound = vio::tcp_bind(server.value(), reinterpret_cast<const sockaddr *>(&addr.value()));
    if (!bound.has_value())
    {
      return fail(status_t::internal_server_error, "tcp_bind: " + bound.error().msg);
    }
  }
  return std::move(server.value());
}

struct bound_server_t
{
  vio::tcp_server_t server;
  std::string host;
};

result_t<bound_server_t> resolve_and_bind(vio::event_loop_t &loop, std::string_view host, uint16_t port, logger_t &logger)
{
  if (host.empty())
  {
    auto dual = bind_one(loop, "::", true, port);
    if (dual.has_value())
    {
      return bound_server_t{std::move(dual.value()), "::"};
    }
    logger.log(log_level_t::warn, "dual-stack bind on [::]:" + std::to_string(port) + " failed (" + dual.error().msg + "); falling back to 0.0.0.0");
    auto ipv4 = bind_one(loop, "0.0.0.0", false, port);
    if (!ipv4.has_value())
    {
      return std::unexpected(ipv4.error());
    }
    return bound_server_t{std::move(ipv4.value()), "0.0.0.0"};
  }
  std::string host_str(host);
  bool ipv6 = vio::ip6_addr(host_str, static_cast<int>(port)).has_value();
  auto server = bind_one(loop, host_str, ipv6, port);
  if (!server.has_value())
  {
    return std::unexpected(server.error());
  }
  return bound_server_t{std::move(server.value()), std::move(host_str)};
}
} // namespace

void app_t::static_files(std::string_view url_prefix, std::string root, bool spa_fallback)
{
  std::string pattern(url_prefix);
  if (pattern.empty() || pattern.back() != '/')
  {
    pattern += '/';
  }
  pattern += "{path...}";
  _router.get(pattern, static_file_handler(std::move(root), "index.html", spa_fallback));
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

  auto bound = resolve_and_bind(loop, host, port, *_logger);
  if (!bound.has_value())
  {
    _logger->log(log_level_t::error, bound.error().msg);
    co_return std::unexpected(bound.error());
  }

  _logger->log(log_level_t::info, "listening on " + bound->host + ":" + std::to_string(port));
  co_return co_await prism::detail::serve(std::move(bound->server), std::make_shared<const router_t>(_router), _logger, cancel, options);
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

  auto bound = resolve_and_bind(loop, host, port, *_logger);
  if (!bound.has_value())
  {
    _logger->log(log_level_t::error, bound.error().msg);
    co_return std::unexpected(bound.error());
  }

  if (config.alpn_protocols.empty())
  {
    config.alpn_protocols = {"h2", "http/1.1"};
  }

  auto tls_server = vio::ssl_server_create(loop, std::move(bound->server), bound->host, config);
  if (!tls_server.has_value())
  {
    _logger->log(log_level_t::error, "ssl_server_create failed: " + tls_server.error().msg);
    co_return fail(status_t::internal_server_error, "ssl_server_create: " + tls_server.error().msg);
  }

  _logger->log(log_level_t::info, "listening on " + bound->host + ":" + std::to_string(port) + " (tls)");
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
