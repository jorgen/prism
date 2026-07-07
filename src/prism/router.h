#pragma once

#include <exception> // vio/task.h uses std::terminate without including this
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vio/task.h>

#include "http.h"

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
  };

  [[nodiscard]] route_match_t resolve(method_t method, std::string_view path) const;
  [[nodiscard]] bool is_streaming(method_t method, std::string_view path) const;

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
    bool streaming = false;
  };

  // Match a split path against a route's segments (ignoring method). Returns the
  // fixed-segment count on success (and whether a trailing wildcard applies), or
  // std::nullopt when the path shape does not match.
  [[nodiscard]] static bool segments_match(const route_t &route, const std::vector<std::string_view> &path_segments);

  std::vector<route_t> _routes;
};
} // namespace prism
