#pragma once

#include <optional>
#include <string>
#include <utility>

#include "codec.h"
#include "content.h"
#include "http.h"
#include "status.h"

namespace prism
{
template <typename T>
struct negotiated_t
{
  status_t status = status_t::ok;
  T value{};
  std::optional<response_t> ready{};

  negotiated_t() = default;
  negotiated_t(status_t status_in, T value_in) : status(status_in), value(std::move(value_in)) {}
  negotiated_t(response_t ready_in) : ready(std::move(ready_in)) {}
};

template <typename T>
negotiated_t<T> ok(T value)
{
  return negotiated_t<T>{status_t::ok, std::move(value)};
}

template <typename T>
negotiated_t<T> created(T value)
{
  return negotiated_t<T>{status_t::created, std::move(value)};
}

template <typename T>
negotiated_t<T> respond(status_t status, T value)
{
  return negotiated_t<T>{status, std::move(value)};
}

namespace detail
{
template <typename T>
response_t render(const request_t &request, negotiated_t<T> negotiated)
{
  if (negotiated.ready)
  {
    return std::move(*negotiated.ready);
  }
  const std::string *accept = request.headers.find("Accept");
  std::optional<format_t> format = accept != nullptr ? negotiate_accept(*accept) : std::optional<format_t>(format_t::json);
  if (!format)
  {
    return response_t::text(status_t::not_acceptable, "Not Acceptable");
  }
  serialized_t serialized = serialize_as(*format, negotiated.value);
  response_t response;
  response.status = negotiated.status;
  response.body = std::move(serialized.bytes);
  response.headers.set("Content-Type", std::string(serialized.media_type));
  return response;
}
} // namespace detail

template <typename T>
response_t respond(const request_t &request, status_t status, const T &value)
{
  return detail::render(request, negotiated_t<T>{status, value});
}
} // namespace prism
