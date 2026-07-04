#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace prism
{
enum class format_t : uint16_t
{
  json = 0,
  yaml = 1,
  cbor = 2,
};

[[nodiscard]] std::string_view media_type_of(format_t format);

[[nodiscard]] std::optional<format_t> negotiate_accept(std::string_view accept_header);

[[nodiscard]] std::optional<format_t> format_for_content_type(std::string_view content_type);
} // namespace prism
