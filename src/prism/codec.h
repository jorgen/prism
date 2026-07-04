#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <structify/structify.h>

#include "content.h"
#include "error.h"
#include "status.h"

namespace prism
{
struct serialized_t
{
  std::string bytes;
  std::string_view media_type;
};

template <typename T>
serialized_t serialize_as(format_t format, const T &value)
{
  switch (format)
  {
  case format_t::yaml:
    return serialized_t{STFY::serializeStructYaml(value), media_type_of(format_t::yaml)};
  case format_t::cbor:
  {
    std::vector<uint8_t> bytes = STFY::serializeStructCbor(value);
    return serialized_t{std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size()), media_type_of(format_t::cbor)};
  }
  case format_t::json:
    break;
  }
  return serialized_t{STFY::serializeStruct(value, STFY::SerializerOptions(STFY::SerializerOptions::Compact)), media_type_of(format_t::json)};
}

template <typename T>
result_t<T> parse_as(format_t format, std::string_view data)
{
  T out{};
  STFY::ParseContext context;
  if (format == format_t::yaml)
  {
    context.tokenizer.allowYaml(true);
  }
  else if (format == format_t::cbor)
  {
    context.tokenizer.allowCbor(true);
  }
  context.tokenizer.addData(data.data(), data.size());
  STFY::Error error = context.parseTo(out);
  if (error != STFY::Error::NoError)
  {
    return fail(status_t::bad_request, context.makeErrorString());
  }
  return out;
}
} // namespace prism
