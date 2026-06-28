#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "status.h"

namespace vio
{
class event_loop_t;
}

namespace prism
{
enum class method_t : uint8_t
{
  get,
  head,
  post,
  put,
  patch,
  del, // "delete" is a keyword
  options,
  unknown,
};

constexpr std::string_view method_name(method_t m)
{
  switch (m)
  {
  case method_t::get:
    return "GET";
  case method_t::head:
    return "HEAD";
  case method_t::post:
    return "POST";
  case method_t::put:
    return "PUT";
  case method_t::patch:
    return "PATCH";
  case method_t::del:
    return "DELETE";
  case method_t::options:
    return "OPTIONS";
  case method_t::unknown:
    return "UNKNOWN";
  }
  return "UNKNOWN";
}

constexpr method_t method_from_name(std::string_view s)
{
  if (s == "GET")
    return method_t::get;
  if (s == "HEAD")
    return method_t::head;
  if (s == "POST")
    return method_t::post;
  if (s == "PUT")
    return method_t::put;
  if (s == "PATCH")
    return method_t::patch;
  if (s == "DELETE")
    return method_t::del;
  if (s == "OPTIONS")
    return method_t::options;
  return method_t::unknown;
}

// A header is a name/value pair. Kept as a flat vector to stay lean and to
// preserve ordering; lookups are linear, which is fine for the small header
// counts typical of REST traffic.
struct header_t
{
  std::string name;
  std::string value;
};

struct headers_t
{
  std::vector<header_t> entries;

  // Case-insensitive lookup. Returns nullptr if absent.
  [[nodiscard]] const std::string *find(std::string_view name) const;
  void set(std::string name, std::string value);
};

struct request_t
{
  method_t method = method_t::get;
  std::string target; // path + optional query, e.g. "/users/42?verbose=1"
  std::string path;   // path component only, e.g. "/users/42"
  headers_t headers;
  std::string body;

  // Path parameters captured by the router, e.g. {"id": "42"} for "/users/{id}".
  std::vector<header_t> params;

  vio::event_loop_t *loop = nullptr;

  [[nodiscard]] std::string_view param(std::string_view name) const;
};

struct response_t
{
  status_t status = status_t::ok;
  headers_t headers;
  std::string body;

  static response_t text(status_t status, std::string body);
};
} // namespace prism
