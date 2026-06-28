#include "prism/http.h"

#include <algorithm>
#include <cctype>

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

response_t response_t::text(status_t status, std::string body)
{
  response_t response;
  response.status = status;
  response.body = std::move(body);
  response.headers.set("Content-Type", "text/plain; charset=utf-8");
  return response;
}
} // namespace prism
