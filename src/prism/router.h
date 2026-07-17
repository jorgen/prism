#pragma once

#include <exception> // vio/task.h uses std::terminate without including this
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vio/task.h>

#include "detail/thread_state.h"
#include "http.h"
#include "websocket.h"

namespace prism
{
// A handler is a coroutine that takes a request and resolves to a response.
// Because it returns vio::task_t it can co_await any async vio operation
// (database calls, outbound HTTP, timers) without blocking the event loop.
using handler_t = std::function<vio::task_t<response_t>(request_t)>;

// A small, allocation-light router. Patterns are matched segment by segment;
// a segment written as "{name}" captures that path segment into
// request_t::params under "name". Matching is linear over registered routes,
// which is more than fast enough for typical REST surfaces.
class router_t
{
public:
  void add(method_t method, std::string_view pattern, handler_t handler, bool streaming = false);

  // Register a WebSocket route (RFC 6455). A matching GET whose request carries a
  // valid Upgrade handshake is hijacked by the server, which runs the ws_handler
  // over the upgraded connection instead of dispatching a normal response.
  void add_websocket(std::string_view pattern, ws_handler_t handler);

  void get(std::string_view pattern, handler_t handler)
  {
    add(method_t::get, pattern, std::move(handler));
  }
  void post(std::string_view pattern, handler_t handler)
  {
    add(method_t::post, pattern, std::move(handler));
  }
  void put(std::string_view pattern, handler_t handler)
  {
    add(method_t::put, pattern, std::move(handler));
  }
  void patch(std::string_view pattern, handler_t handler)
  {
    add(method_t::patch, pattern, std::move(handler));
  }
  void del(std::string_view pattern, handler_t handler)
  {
    add(method_t::del, pattern, std::move(handler));
  }

  // Streaming variants: the handler is dispatched at headers-complete and pulls
  // the body via request_t::body_reader instead of receiving it fully buffered.
  void get_stream(std::string_view pattern, handler_t handler)
  {
    add(method_t::get, pattern, std::move(handler), true);
  }
  void post_stream(std::string_view pattern, handler_t handler)
  {
    add(method_t::post, pattern, std::move(handler), true);
  }
  void put_stream(std::string_view pattern, handler_t handler)
  {
    add(method_t::put, pattern, std::move(handler), true);
  }
  void patch_stream(std::string_view pattern, handler_t handler)
  {
    add(method_t::patch, pattern, std::move(handler), true);
  }
  void del_stream(std::string_view pattern, handler_t handler)
  {
    add(method_t::del, pattern, std::move(handler), true);
  }

  // Find a matching route and run its handler. Yields a 404 when no path
  // matches and a 405 when the path matches but the method does not.
  [[nodiscard]] vio::task_t<response_t> dispatch(request_t request) const;

  // Outcome of matching a (method, path) without running the handler. The server
  // uses this at headers-complete to decide buffered vs streaming dispatch.
  struct route_match_t
  {
    bool path_matched = false;   // some route matched the path
    bool method_allowed = false; // a route matched path AND method
    bool streaming = false;      // the matched route streams its request body
    bool websocket = false;      // the matched route is a WebSocket route
  };

  [[nodiscard]] route_match_t resolve(method_t method, std::string_view path) const;
  [[nodiscard]] bool is_streaming(method_t method, std::string_view path) const;

  // Bind the WebSocket route matching request.path (GET), stamping request.params
  // + request.factories exactly as dispatch would, and returning its handler. An
  // empty handler means no WebSocket route matched.
  [[nodiscard]] ws_handler_t match_websocket(request_t &request) const;

  // Register a factory that produces one instance of T per thread (per event
  // loop). Handlers receive it via a prism::per_thread<T> parameter. Call before
  // listen(); one factory per type (last registration for a type wins).
  template <typename T, typename F>
  void provide_per_thread(F factory)
  {
    _factories->template provide<T>(std::move(factory));
  }

private:
  struct segment_t
  {
    std::string text; // literal text, or the parameter name when is_param/is_wildcard
    bool is_param = false;
    bool is_wildcard = false; // "{name...}" as the final segment: captures the rest of the path
  };

  struct route_t
  {
    method_t method;
    std::vector<segment_t> segments;
    handler_t handler;
    ws_handler_t ws_handler;
    bool streaming = false;
    bool websocket = false;
  };

  [[nodiscard]] static route_t make_route(method_t method, std::string_view pattern);

  // Match a split path against a route's segments (ignoring method). Returns the
  // fixed-segment count on success (and whether a trailing wildcard applies), or
  // std::nullopt when the path shape does not match.
  [[nodiscard]] static bool segments_match(const route_t &route, const std::vector<std::string_view> &path_segments);

  std::vector<route_t> _routes;
  std::shared_ptr<detail::per_thread_registry_t> _factories = std::make_shared<detail::per_thread_registry_t>();
};
} // namespace prism
