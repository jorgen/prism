#pragma once

#include <cstdint>
#include <string_view>

namespace prism
{
// HTTP status codes. Only the common ones are spelled out; any uint16_t is
// valid as a status value.
enum class status_t : uint16_t
{
  ok = 200,
  created = 201,
  accepted = 202,
  no_content = 204,
  moved_permanently = 301,
  found = 302,
  not_modified = 304,
  bad_request = 400,
  unauthorized = 401,
  forbidden = 403,
  not_found = 404,
  method_not_allowed = 405,
  not_acceptable = 406,
  conflict = 409,
  payload_too_large = 413,
  unsupported_media_type = 415,
  unprocessable_entity = 422,
  too_many_requests = 429,
  request_header_fields_too_large = 431,
  internal_server_error = 500,
  not_implemented = 501,
  bad_gateway = 502,
  service_unavailable = 503,
};

// Standard reason phrase for a status code, or "Unknown" if not recognised.
constexpr std::string_view reason_phrase(status_t s)
{
  switch (s)
  {
  case status_t::ok:
    return "OK";
  case status_t::created:
    return "Created";
  case status_t::accepted:
    return "Accepted";
  case status_t::no_content:
    return "No Content";
  case status_t::moved_permanently:
    return "Moved Permanently";
  case status_t::found:
    return "Found";
  case status_t::not_modified:
    return "Not Modified";
  case status_t::bad_request:
    return "Bad Request";
  case status_t::unauthorized:
    return "Unauthorized";
  case status_t::forbidden:
    return "Forbidden";
  case status_t::not_found:
    return "Not Found";
  case status_t::method_not_allowed:
    return "Method Not Allowed";
  case status_t::not_acceptable:
    return "Not Acceptable";
  case status_t::conflict:
    return "Conflict";
  case status_t::payload_too_large:
    return "Payload Too Large";
  case status_t::unsupported_media_type:
    return "Unsupported Media Type";
  case status_t::unprocessable_entity:
    return "Unprocessable Entity";
  case status_t::too_many_requests:
    return "Too Many Requests";
  case status_t::request_header_fields_too_large:
    return "Request Header Fields Too Large";
  case status_t::internal_server_error:
    return "Internal Server Error";
  case status_t::not_implemented:
    return "Not Implemented";
  case status_t::bad_gateway:
    return "Bad Gateway";
  case status_t::service_unavailable:
    return "Service Unavailable";
  }
  return "Unknown";
}
} // namespace prism
