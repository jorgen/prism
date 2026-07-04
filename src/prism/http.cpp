#include "prism/http.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>

namespace prism
{
namespace
{
bool iequals(std::string_view a, std::string_view b)
{
  if (a.size() != b.size())
  {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i)
  {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
    {
      return false;
    }
  }
  return true;
}

bool is_hex_digit(char c)
{
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int hex_value(char c)
{
  if (c >= '0' && c <= '9')
  {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f')
  {
    return c - 'a' + 10;
  }
  return c - 'A' + 10;
}

std::string percent_decode(std::string_view in)
{
  std::string out;
  out.reserve(in.size());
  for (std::size_t i = 0; i < in.size(); ++i)
  {
    char c = in[i];
    if (c == '+')
    {
      out.push_back(' ');
    }
    else if (c == '%' && i + 2 < in.size() && is_hex_digit(in[i + 1]) && is_hex_digit(in[i + 2]))
    {
      out.push_back(static_cast<char>((hex_value(in[i + 1]) << 4) | hex_value(in[i + 2])));
      i += 2;
    }
    else
    {
      out.push_back(c);
    }
  }
  return out;
}

std::optional<std::string> find_query(std::string_view target, std::string_view name)
{
  std::size_t mark = target.find('?');
  if (mark == std::string_view::npos)
  {
    return std::nullopt;
  }
  std::string_view rest = target.substr(mark + 1);
  while (!rest.empty())
  {
    std::size_t amp = rest.find('&');
    std::string_view pair = amp == std::string_view::npos ? rest : rest.substr(0, amp);
    std::size_t eq = pair.find('=');
    std::string_view key = eq == std::string_view::npos ? pair : pair.substr(0, eq);
    if (name == percent_decode(key))
    {
      std::string_view value = eq == std::string_view::npos ? std::string_view{} : pair.substr(eq + 1);
      return percent_decode(value);
    }
    if (amp == std::string_view::npos)
    {
      break;
    }
    rest = rest.substr(amp + 1);
  }
  return std::nullopt;
}
} // namespace

const std::string *headers_t::find(std::string_view name) const
{
  for (const auto &entry : entries)
  {
    if (iequals(entry.name, name))
    {
      return &entry.value;
    }
  }
  return nullptr;
}

void headers_t::set(std::string name, std::string value)
{
  for (auto &entry : entries)
  {
    if (iequals(entry.name, name))
    {
      entry.value = std::move(value);
      return;
    }
  }
  entries.push_back(header_t{std::move(name), std::move(value)});
}

std::string_view request_t::param(std::string_view name) const
{
  for (const auto &entry : params)
  {
    if (entry.name == name)
    {
      return entry.value;
    }
  }
  return {};
}

std::string request_t::query(std::string_view name) const
{
  return find_query(target, name).value_or(std::string{});
}

bool request_t::has_query(std::string_view name) const
{
  return find_query(target, name).has_value();
}

std::span<const std::byte> request_t::raw_body() const
{
  return {reinterpret_cast<const std::byte *>(body.data()), body.size()};
}

response_t response_t::text(status_t status, std::string body)
{
  response_t response;
  response.status = status;
  response.body = std::move(body);
  response.headers.set("Content-Type", "text/plain; charset=utf-8");
  return response;
}

response_t response_t::finished(status_t status, std::string content_type, std::string bytes)
{
  response_t response;
  response.status = status;
  response.body = std::move(bytes);
  response.headers.set("Content-Type", std::move(content_type));
  return response;
}
} // namespace prism
