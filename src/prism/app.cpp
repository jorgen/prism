#include "prism/app.h"

#include <cstdint>
#include <latch>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <vio/cancellation.h>
#include <vio/event_loop.h>
#include <vio/operation/tcp.h>
#include <vio/operation/tcp_server.h>
#include <vio/operation/tls_server.h>

#include "prism/detail/server.h"
#include "prism/static_files.h"

namespace prism
{
namespace
{
result_t<vio::tcp_server_t> bind_one(vio::event_loop_t &loop, const std::string &host, bool ipv6, uint16_t port, bool reuseport)
{
  auto server = vio::tcp_create_server(loop);
  if (!server.has_value())
  {
    return fail(status_t::internal_server_error, "tcp_create_server: " + server.error().msg);
  }
  const unsigned int flags = reuseport ? static_cast<unsigned int>(UV_TCP_REUSEPORT) : 0u;
  if (ipv6)
  {
    auto addr = vio::ip6_addr(host, static_cast<int>(port));
    if (!addr.has_value())
    {
      return fail(status_t::internal_server_error, "ip6_addr: " + addr.error().msg);
    }
    auto bound = vio::tcp_bind(server.value(), reinterpret_cast<const sockaddr *>(&addr.value()), flags);
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
    auto bound = vio::tcp_bind(server.value(), reinterpret_cast<const sockaddr *>(&addr.value()), flags);
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
  bool ipv6 = false;
};

result_t<bound_server_t> resolve_and_bind(vio::event_loop_t &loop, std::string_view host, uint16_t port, bool reuseport, logger_t &logger)
{
  if (host.empty())
  {
    auto dual = bind_one(loop, "::", true, port, reuseport);
    if (dual.has_value())
    {
      return bound_server_t{std::move(dual.value()), "::", true};
    }
    logger.log(log_level_t::warn, "dual-stack bind on [::]:" + std::to_string(port) + " failed (" + dual.error().msg + "); falling back to 0.0.0.0");
    auto ipv4 = bind_one(loop, "0.0.0.0", false, port, reuseport);
    if (!ipv4.has_value())
    {
      return std::unexpected(ipv4.error());
    }
    return bound_server_t{std::move(ipv4.value()), "0.0.0.0", false};
  }
  std::string host_str(host);
  bool ipv6 = vio::ip6_addr(host_str, static_cast<int>(port)).has_value();
  auto server = bind_one(loop, host_str, ipv6, port, reuseport);
  if (!server.has_value())
  {
    return std::unexpected(server.error());
  }
  return bound_server_t{std::move(server.value()), std::move(host_str), ipv6};
}

struct worker_t
{
  std::unique_ptr<vio::thread_with_event_loop_t> thread;
  vio::cancellation_t cancel;
};

std::uint32_t effective_worker_count(std::uint32_t requested)
{
  if (requested != 0)
  {
    return requested;
  }
  std::uint32_t hardware = std::thread::hardware_concurrency();
  return hardware == 0 ? 1 : hardware;
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

vio::task_t<result_t<void>> app_t::listen(vio::event_loop_t &loop, std::string_view host, uint16_t port, vio::cancellation_t *cancel, server_options_t options)
{
  if (!_route_errors.empty())
  {
    for (const auto &route_error : _route_errors)
    {
      _logger->log(log_level_t::error, route_error);
    }
    co_return fail(status_t::internal_server_error, "route configuration error: " + _route_errors.front());
  }

  const std::uint32_t workers = effective_worker_count(options.worker_threads);
  const bool reuseport = workers > 1;

  auto bound = resolve_and_bind(loop, host, port, reuseport, *_logger);
  if (!bound.has_value())
  {
    _logger->log(log_level_t::error, bound.error().msg);
    co_return std::unexpected(bound.error());
  }

  auto routes = std::make_shared<const router_t>(_router);
  const std::string host_resolved = bound->host;
  const bool ipv6 = bound->ipv6;

  if (workers == 1)
  {
    _logger->log(log_level_t::info, "listening on " + host_resolved + ":" + std::to_string(port));
    co_return co_await prism::detail::serve(std::move(bound->server), std::move(routes), _logger, cancel, options);
  }

  std::vector<std::unique_ptr<worker_t>> pool;
  pool.reserve(workers - 1);
  std::latch drained(static_cast<std::ptrdiff_t>(workers - 1));

  for (std::uint32_t i = 1; i < workers; ++i)
  {
    auto worker = std::make_unique<worker_t>();
    worker->thread = std::make_unique<vio::thread_with_event_loop_t>();
    vio::event_loop_t *worker_loop = &worker->thread->event_loop();
    vio::cancellation_t *worker_cancel = &worker->cancel;
    auto logger = _logger;
    worker_loop->run_in_loop([worker_loop, worker_cancel, host_resolved, ipv6, port, routes, logger, options, &drained]()
                             {
                               auto server = bind_one(*worker_loop, host_resolved, ipv6, port, true);
                               if (!server.has_value())
                               {
                                 logger->log(log_level_t::error, "worker bind failed: " + server.error().msg);
                                 drained.count_down();
                                 return;
                               }
                               [](vio::tcp_server_t bound_server, std::shared_ptr<const router_t> routes_ref, std::shared_ptr<logger_t> logger_ref, vio::cancellation_t *worker_cancel_ref, server_options_t options_ref, std::latch *drained_ref) -> vio::detached_task_t
                               {
                                 co_await prism::detail::serve(std::move(bound_server), std::move(routes_ref), std::move(logger_ref), worker_cancel_ref, options_ref);
                                 drained_ref->count_down();
                               }(std::move(server.value()), routes, logger, worker_cancel, options, &drained);
                             });
    pool.push_back(std::move(worker));
  }

  auto stop_workers = [&pool]()
  {
    for (auto &worker : pool)
    {
      vio::event_loop_t *worker_loop = &worker->thread->event_loop();
      vio::cancellation_t *worker_cancel = &worker->cancel;
      worker_loop->run_in_loop([worker_cancel]() { worker_cancel->cancel(); });
    }
  };

  vio::registration_t fanout;
  if (cancel != nullptr)
  {
    fanout = cancel->register_callback(stop_workers);
  }

  _logger->log(log_level_t::info, "listening on " + host_resolved + ":" + std::to_string(port) + " (workers=" + std::to_string(workers) + ")");
  auto result = co_await prism::detail::serve(std::move(bound->server), routes, _logger, cancel, options);

  stop_workers();
  drained.wait();
  pool.clear();
  co_return result;
}

vio::task_t<result_t<void>> app_t::listen_tls(vio::event_loop_t &loop, std::string_view host, uint16_t port, vio::ssl_config_t config, vio::cancellation_t *cancel, server_options_t options)
{
  if (!_route_errors.empty())
  {
    for (const auto &route_error : _route_errors)
    {
      _logger->log(log_level_t::error, route_error);
    }
    co_return fail(status_t::internal_server_error, "route configuration error: " + _route_errors.front());
  }

  if (config.alpn_protocols.empty())
  {
    config.alpn_protocols = {"h2", "http/1.1"};
  }

  const std::uint32_t workers = effective_worker_count(options.worker_threads);
  const bool reuseport = workers > 1;

  auto bound = resolve_and_bind(loop, host, port, reuseport, *_logger);
  if (!bound.has_value())
  {
    _logger->log(log_level_t::error, bound.error().msg);
    co_return std::unexpected(bound.error());
  }

  auto routes = std::make_shared<const router_t>(_router);
  const std::string host_resolved = bound->host;
  const bool ipv6 = bound->ipv6;

  auto tls_server = vio::ssl_server_create(loop, std::move(bound->server), host_resolved, config);
  if (!tls_server.has_value())
  {
    _logger->log(log_level_t::error, "ssl_server_create failed: " + tls_server.error().msg);
    co_return fail(status_t::internal_server_error, "ssl_server_create: " + tls_server.error().msg);
  }

  if (workers == 1)
  {
    _logger->log(log_level_t::info, "listening on " + host_resolved + ":" + std::to_string(port) + " (tls)");
    co_return co_await prism::detail::serve_tls(std::move(tls_server.value()), std::move(routes), _logger, cancel, options);
  }

  std::vector<std::unique_ptr<worker_t>> pool;
  pool.reserve(workers - 1);
  std::latch drained(static_cast<std::ptrdiff_t>(workers - 1));

  for (std::uint32_t i = 1; i < workers; ++i)
  {
    auto worker = std::make_unique<worker_t>();
    worker->thread = std::make_unique<vio::thread_with_event_loop_t>();
    vio::event_loop_t *worker_loop = &worker->thread->event_loop();
    vio::cancellation_t *worker_cancel = &worker->cancel;
    auto logger = _logger;
    worker_loop->run_in_loop([worker_loop, worker_cancel, host_resolved, ipv6, port, routes, logger, options, config, &drained]()
                             {
                               auto server = bind_one(*worker_loop, host_resolved, ipv6, port, true);
                               if (!server.has_value())
                               {
                                 logger->log(log_level_t::error, "worker bind failed: " + server.error().msg);
                                 drained.count_down();
                                 return;
                               }
                               auto tls = vio::ssl_server_create(*worker_loop, std::move(server.value()), host_resolved, config);
                               if (!tls.has_value())
                               {
                                 logger->log(log_level_t::error, "worker ssl_server_create failed: " + tls.error().msg);
                                 drained.count_down();
                                 return;
                               }
                               [](vio::ssl_server_t tls_server_ref, std::shared_ptr<const router_t> routes_ref, std::shared_ptr<logger_t> logger_ref, vio::cancellation_t *worker_cancel_ref, server_options_t options_ref, std::latch *drained_ref) -> vio::detached_task_t
                               {
                                 co_await prism::detail::serve_tls(std::move(tls_server_ref), std::move(routes_ref), std::move(logger_ref), worker_cancel_ref, options_ref);
                                 drained_ref->count_down();
                               }(std::move(tls.value()), routes, logger, worker_cancel, options, &drained);
                             });
    pool.push_back(std::move(worker));
  }

  auto stop_workers = [&pool]()
  {
    for (auto &worker : pool)
    {
      vio::event_loop_t *worker_loop = &worker->thread->event_loop();
      vio::cancellation_t *worker_cancel = &worker->cancel;
      worker_loop->run_in_loop([worker_cancel]() { worker_cancel->cancel(); });
    }
  };

  vio::registration_t fanout;
  if (cancel != nullptr)
  {
    fanout = cancel->register_callback(stop_workers);
  }

  _logger->log(log_level_t::info, "listening on " + host_resolved + ":" + std::to_string(port) + " (tls, workers=" + std::to_string(workers) + ")");
  auto result = co_await prism::detail::serve_tls(std::move(tls_server.value()), routes, _logger, cancel, options);

  stop_workers();
  drained.wait();
  pool.clear();
  co_return result;
}

vio::task_t<int> run(vio::event_loop_t &loop, std::string_view host, uint16_t port, std::function<void(app_t &)> configure, server_options_t options)
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
