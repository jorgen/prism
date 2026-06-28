#pragma once

#include <string>
#include <string_view>

#include <structify/structify.h>

#include "error.h"
#include "http.h"

// Glue between prism and structify: turn serializable C++ structs (those with
// STFY_OBJ / STFY_OBJ_EXT metadata) into JSON bodies and back, with parse
// failures surfaced as prism::result_t errors carrying a 400 status.
namespace prism::json
{
template <typename T>
std::string serialize(const T &value, bool pretty = false)
{
  if (pretty)
  {
    return STFY::serializeStruct(value);
  }
  return STFY::serializeStruct(value, STFY::SerializerOptions(STFY::SerializerOptions::Compact));
}

template <typename T>
result_t<T> parse(std::string_view data)
{
  T out{};
  STFY::ParseContext context(data.data(), data.size());
  STFY::Error error = context.parseTo(out);
  if (error != STFY::Error::NoError)
  {
    return fail(status_t::bad_request, context.makeErrorString());
  }
  return out;
}

// Build an application/json response from a serializable struct.
template <typename T>
response_t respond(status_t status, const T &value)
{
  response_t response;
  response.status = status;
  response.body = serialize(value);
  response.headers.set("Content-Type", "application/json");
  return response;
}
} // namespace prism::json
