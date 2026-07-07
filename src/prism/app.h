#pragma once

#include <cstdint>
#include <exception> // vio/task.h uses std::terminate without including this
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <vio/ssl_config_t.h>
#include <vio/task.h>

#include "error.h"
#include "logging.h"
#include "params.h"
#include "router.h"
#include "server_options.h"

namespace vio
{
class event_loop_t;
class cancellation_t;
}

namespace prism
{
// app_t is the front door: register routes, then listen. It is a thin facade
// over router_t plus the (forthcoming) HTTP/1.1 server that drives requests
// through it on a vio event loop.
class app_t
{
public:
  void get(std::string_view pattern, handler_t handler)
  {
    _router.get(pattern, std::move(handler));
  }
  void post(std::string_view pattern, handler_t handler)
  {
    _router.post(pattern, std::move(handler));
  }
  void put(std::string_view pattern, handler_t handler)
  {
    _router.put(pattern, std::move(handler));
  }
  void patch(std::string_view pattern, handler_t handler)
  {
    _router.patch(pattern, std::move(handler));
  }
  void del(std::string_view pattern, handler_t handler)
  {
    _router.del(pattern, std::move(handler));
  }

  // Streaming-body routes: the handler is dispatched once the headers are parsed
  // and pulls the (possibly very large) body through request_t::body_reader,
  // without prism buffering it whole. Streaming handlers take a raw request_t
  // (typed body_t<T> extraction requires a fully-buffered body).
  void get_stream(std::string_view pattern, handler_t handler)
  {
    _router.get_stream(pattern, std::move(handler));
  }
  void post_stream(std::string_view pattern, handler_t handler)
  {
    _router.post_stream(pattern, std::move(handler));
  }
  void put_stream(std::string_view pattern, handler_t handler)
  {
    _router.put_stream(pattern, std::move(handler));
  }
  void patch_stream(std::string_view pattern, handler_t handler)
  {
    _router.patch_stream(pattern, std::move(handler));
  }
  void del_stream(std::string_view pattern, handler_t handler)
  {
    _router.del_stream(pattern, std::move(handler));
  }

  template <typename Handler, typename... Bound>
    requires(!std::is_convertible_v<std::decay_t<Handler>, handler_t>)
  void get(std::string_view pattern, Handler &&handler, Bound &&...bound)
  {
    record_route_error(detail::verify_routes<std::decay_t<Handler>, sizeof...(Bound)>(pattern));
    _router.get(pattern, detail::make_typed_handler(std::forward<Handler>(handler), std::forward<Bound>(bound)...));
  }
  template <typename Handler, typename... Bound>
    requires(!std::is_convertible_v<std::decay_t<Handler>, handler_t>)
  void post(std::string_view pattern, Handler &&handler, Bound &&...bound)
  {
    record_route_error(detail::verify_routes<std::decay_t<Handler>, sizeof...(Bound)>(pattern));
    _router.post(pattern, detail::make_typed_handler(std::forward<Handler>(handler), std::forward<Bound>(bound)...));
  }
  template <typename Handler, typename... Bound>
    requires(!std::is_convertible_v<std::decay_t<Handler>, handler_t>)
  void put(std::string_view pattern, Handler &&handler, Bound &&...bound)
  {
    record_route_error(detail::verify_routes<std::decay_t<Handler>, sizeof...(Bound)>(pattern));
    _router.put(pattern, detail::make_typed_handler(std::forward<Handler>(handler), std::forward<Bound>(bound)...));
  }
  template <typename Handler, typename... Bound>
    requires(!std::is_convertible_v<std::decay_t<Handler>, handler_t>)
  void patch(std::string_view pattern, Handler &&handler, Bound &&...bound)
  {
    record_route_error(detail::verify_routes<std::decay_t<Handler>, sizeof...(Bound)>(pattern));
    _router.patch(pattern, detail::make_typed_handler(std::forward<Handler>(handler), std::forward<Bound>(bound)...));
  }
  template <typename Handler, typename... Bound>
    requires(!std::is_convertible_v<std::decay_t<Handler>, handler_t>)
  void del(std::string_view pattern, Handler &&handler, Bound &&...bound)
  {
    record_route_error(detail::verify_routes<std::decay_t<Handler>, sizeof...(Bound)>(pattern));
    _router.del(pattern, detail::make_typed_handler(std::forward<Handler>(handler), std::forward<Bound>(bound)...));
  }

  template <fixed_string_t Pattern, typename Handler, typename... Bound>
    requires(!std::is_convertible_v<std::decay_t<Handler>, handler_t>)
  void get(Handler &&handler, Bound &&...bound)
  {
    detail::verify_routes_static<Pattern, std::decay_t<Handler>, sizeof...(Bound)>();
    _router.get(Pattern.view(), detail::make_typed_handler(std::forward<Handler>(handler), std::forward<Bound>(bound)...));
  }
  template <fixed_string_t Pattern, typename Handler, typename... Bound>
    requires(!std::is_convertible_v<std::decay_t<Handler>, handler_t>)
  void post(Handler &&handler, Bound &&...bound)
  {
    detail::verify_routes_static<Pattern, std::decay_t<Handler>, sizeof...(Bound)>();
    _router.post(Pattern.view(), detail::make_typed_handler(std::forward<Handler>(handler), std::forward<Bound>(bound)...));
  }
  template <fixed_string_t Pattern, typename Handler, typename... Bound>
    requires(!std::is_convertible_v<std::decay_t<Handler>, handler_t>)
  void put(Handler &&handler, Bound &&...bound)
  {
    detail::verify_routes_static<Pattern, std::decay_t<Handler>, sizeof...(Bound)>();
    _router.put(Pattern.view(), detail::make_typed_handler(std::forward<Handler>(handler), std::forward<Bound>(bound)...));
  }
  template <fixed_string_t Pattern, typename Handler, typename... Bound>
    requires(!std::is_convertible_v<std::decay_t<Handler>, handler_t>)
  void patch(Handler &&handler, Bound &&...bound)
  {
    detail::verify_routes_static<Pattern, std::decay_t<Handler>, sizeof...(Bound)>();
    _router.patch(Pattern.view(), detail::make_typed_handler(std::forward<Handler>(handler), std::forward<Bound>(bound)...));
  }
  template <fixed_string_t Pattern, typename Handler, typename... Bound>
    requires(!std::is_convertible_v<std::decay_t<Handler>, handler_t>)
  void del(Handler &&handler, Bound &&...bound)
  {
    detail::verify_routes_static<Pattern, std::decay_t<Handler>, sizeof...(Bound)>();
    _router.del(Pattern.view(), detail::make_typed_handler(std::forward<Handler>(handler), std::forward<Bound>(bound)...));
  }

  [[nodiscard]] const router_t &router() const
  {
    return _router;
  }

  [[nodiscard]] const std::vector<std::string> &route_errors() const
  {
    return _route_errors;
  }

  [[nodiscard]] logger_t &logger()
  {
    return *_logger;
  }
  [[nodiscard]] const logger_t &logger() const
  {
    return *_logger;
  }
  void set_logger(std::shared_ptr<logger_t> logger)
  {
    _logger = logger ? std::move(logger) : std::make_shared<logger_t>();
  }

  // Serve files from `root` under the `url_prefix` path (e.g. static_files("/",
  // "webroot") or static_files("/assets", "dist/assets")). Registers a wildcard
  // GET route, so register your REST routes first — the first match wins, and a
  // root ("/") mount otherwise catches everything. Set `spa_fallback` to serve
  // index.html for unmatched navigation paths (single-page-app client routing).
  void static_files(std::string_view url_prefix, std::string root, bool spa_fallback = false);

  // Run a single request through the router. Useful for tests and for
  // embedding prism behind another transport.
  [[nodiscard]] vio::task_t<response_t> handle(request_t request) const
  {
    return _router.dispatch(std::move(request));
  }

  [[nodiscard]] vio::task_t<result_t<void>> listen(vio::event_loop_t &loop, std::string_view host, uint16_t port, vio::cancellation_t *cancel = nullptr, keepalive_options_t options = {});

  [[nodiscard]] vio::task_t<result_t<void>> listen_tls(vio::event_loop_t &loop, std::string_view host, uint16_t port, vio::ssl_config_t config, vio::cancellation_t *cancel = nullptr, keepalive_options_t options = {});

private:
  void record_route_error(std::optional<std::string> error)
  {
    if (error)
    {
      _route_errors.push_back(std::move(*error));
    }
  }

  router_t _router;
  std::shared_ptr<logger_t> _logger = std::make_shared<logger_t>();
  std::vector<std::string> _route_errors;
};

[[nodiscard]] vio::task_t<int> run(vio::event_loop_t &loop, std::string_view host, uint16_t port, std::function<void(app_t &)> configure, keepalive_options_t options = {});
} // namespace prism
