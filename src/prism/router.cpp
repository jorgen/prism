#include "prism/router.h"

#include <string_view>

namespace prism
{
namespace
{
// Split a '/'-delimited path into its non-empty segments. "/users/42" yields
// {"users", "42"}; "/" and "" both yield {}.
std::vector<std::string_view> split_segments(std::string_view path)
{
  std::vector<std::string_view> out;
  size_t i = 0;
  while (i < path.size())
  {
    if (path[i] == '/')
    {
      ++i;
      continue;
    }
    size_t start = i;
    while (i < path.size() && path[i] != '/')
    {
      ++i;
    }
    out.push_back(path.substr(start, i - start));
  }
  return out;
}
} // namespace

router_t::route_t router_t::make_route(method_t method, std::string_view pattern)
{
  route_t route;
  route.method = method;
  for (std::string_view seg : split_segments(pattern))
  {
    segment_t segment;
    if (seg.size() >= 2 && seg.front() == '{' && seg.back() == '}')
    {
      std::string_view inner = seg.substr(1, seg.size() - 2);
      if (inner.size() >= 3 && inner.substr(inner.size() - 3) == "...")
      {
        segment.is_wildcard = true;
        segment.text = std::string(inner.substr(0, inner.size() - 3));
      }
      else
      {
        segment.is_param = true;
        segment.text = std::string(inner);
      }
    }
    else
    {
      segment.text = std::string(seg);
    }
    route.segments.push_back(std::move(segment));
  }
  return route;
}

void router_t::add(method_t method, std::string_view pattern, handler_t handler, bool streaming)
{
  route_t route = make_route(method, pattern);
  route.handler = std::move(handler);
  route.streaming = streaming;
  _routes.push_back(std::move(route));
}

void router_t::add_websocket(std::string_view pattern, ws_handler_t handler)
{
  route_t route = make_route(method_t::get, pattern);
  route.ws_handler = std::move(handler);
  route.websocket = true;
  _routes.push_back(std::move(route));
}

bool router_t::segments_match(const route_t &route, const std::vector<std::string_view> &path_segments)
{
  const bool has_wildcard = !route.segments.empty() && route.segments.back().is_wildcard;
  const size_t fixed = has_wildcard ? route.segments.size() - 1 : route.segments.size();

  if (has_wildcard ? path_segments.size() < fixed : path_segments.size() != fixed)
  {
    return false;
  }
  for (size_t i = 0; i < fixed; ++i)
  {
    const segment_t &pattern_seg = route.segments[i];
    if (!pattern_seg.is_param && pattern_seg.text != path_segments[i])
    {
      return false;
    }
  }
  return true;
}

router_t::route_match_t router_t::resolve(method_t method, std::string_view path) const
{
  std::vector<std::string_view> path_segments = split_segments(path);
  route_match_t result;
  for (const auto &route : _routes)
  {
    if (!segments_match(route, path_segments))
    {
      continue;
    }
    result.path_matched = true;
    if (route.method == method)
    {
      result.method_allowed = true;
      result.streaming = route.streaming;
      result.websocket = route.websocket;
      return result;
    }
  }
  return result;
}

bool router_t::is_streaming(method_t method, std::string_view path) const
{
  return resolve(method, path).streaming;
}

bool router_t::has_websocket_routes() const
{
  for (const auto &route : _routes)
  {
    if (route.websocket)
    {
      return true;
    }
  }
  return false;
}

ws_handler_t router_t::match_websocket(request_t &request) const
{
  request.factories = _factories.get();
  std::vector<std::string_view> path_segments = split_segments(request.path);
  for (const auto &route : _routes)
  {
    if (!route.websocket || route.method != request.method || !segments_match(route, path_segments))
    {
      continue;
    }

    const bool has_wildcard = !route.segments.empty() && route.segments.back().is_wildcard;
    const size_t fixed = has_wildcard ? route.segments.size() - 1 : route.segments.size();

    for (size_t i = 0; i < fixed; ++i)
    {
      const segment_t &pattern_seg = route.segments[i];
      if (pattern_seg.is_param)
      {
        request.params.push_back(header_t{pattern_seg.text, std::string(path_segments[i])});
      }
    }
    if (has_wildcard)
    {
      std::string tail;
      for (size_t i = fixed; i < path_segments.size(); ++i)
      {
        if (i > fixed)
        {
          tail += '/';
        }
        tail += path_segments[i];
      }
      request.params.push_back(header_t{route.segments.back().text, std::move(tail)});
    }
    return route.ws_handler;
  }
  return ws_handler_t{};
}

vio::task_t<response_t> router_t::dispatch(request_t request) const
{
  request.factories = _factories.get();
  std::vector<std::string_view> path_segments = split_segments(request.path);
  bool path_matched = false;

  for (const auto &route : _routes)
  {
    if (route.websocket || !segments_match(route, path_segments))
    {
      continue;
    }

    const bool has_wildcard = !route.segments.empty() && route.segments.back().is_wildcard;
    const size_t fixed = has_wildcard ? route.segments.size() - 1 : route.segments.size();

    path_matched = true;
    if (route.method != request.method)
    {
      continue;
    }

    // Bind captured path parameters before handing off to the handler.
    for (size_t i = 0; i < fixed; ++i)
    {
      const segment_t &pattern_seg = route.segments[i];
      if (pattern_seg.is_param)
      {
        request.params.push_back(header_t{pattern_seg.text, std::string(path_segments[i])});
      }
    }

    // A trailing "{name...}" captures the remainder of the path, '/'-joined.
    if (has_wildcard)
    {
      std::string tail;
      for (size_t i = fixed; i < path_segments.size(); ++i)
      {
        if (i > fixed)
        {
          tail += '/';
        }
        tail += path_segments[i];
      }
      request.params.push_back(header_t{route.segments.back().text, std::move(tail)});
    }

    co_return co_await route.handler(std::move(request));
  }

  if (path_matched)
  {
    co_return response_t::text(status_t::method_not_allowed, "Method Not Allowed");
  }
  co_return response_t::text(status_t::not_found, "Not Found");
}
} // namespace prism
