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
  void add(method_t method, std::string_view pattern, handler_t handler);

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

  // Find a matching route and run its handler. Yields a 404 when no path
  // matches and a 405 when the path matches but the method does not.
  [[nodiscard]] vio::task_t<response_t> dispatch(request_t request) const;

private:
  struct segment_t
  {
    std::string text; // literal text, or the parameter name when is_param
    bool is_param = false;
  };

  struct route_t
  {
    method_t method;
    std::vector<segment_t> segments;
    handler_t handler;
  };

  std::vector<route_t> _routes;
};
} // namespace prism
