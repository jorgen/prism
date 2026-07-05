#include "prism/static_files.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include <sys/stat.h>

#include <vio/event_loop.h>
#include <vio/operation/file.h>

namespace prism
{
namespace
{
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
  if (c >= 'A' && c <= 'F')
  {
    return c - 'A' + 10;
  }
  return -1;
}

std::string percent_decode(std::string_view value)
{
  std::string out;
  out.reserve(value.size());
  for (std::size_t i = 0; i < value.size(); ++i)
  {
    if (value[i] == '%' && i + 2 < value.size())
    {
      int hi = hex_value(value[i + 1]);
      int lo = hex_value(value[i + 2]);
      if (hi >= 0 && lo >= 0)
      {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    out.push_back(value[i]);
  }
  return out;
}

// Normalise a client-supplied relative path into a safe one under the root.
// Rejects ".." (traversal) and NUL; collapses separators; drops "." segments.
bool safe_relative(std::string_view input, std::string &out)
{
  out.clear();
  std::size_t i = 0;
  while (i < input.size())
  {
    while (i < input.size() && (input[i] == '/' || input[i] == '\\'))
    {
      ++i;
    }
    std::size_t start = i;
    while (i < input.size() && input[i] != '/' && input[i] != '\\')
    {
      ++i;
    }
    std::string_view segment = input.substr(start, i - start);
    if (segment.empty() || segment == ".")
    {
      continue;
    }
    if (segment == "..")
    {
      return false;
    }
    for (char c : segment)
    {
      if (c == '\0')
      {
        return false;
      }
    }
    if (!out.empty())
    {
      out += '/';
    }
    out += segment;
  }
  return true;
}
} // namespace

std::string_view content_type_for_path(std::string_view path)
{
  std::size_t dot = path.find_last_of('.');
  std::size_t slash = path.find_last_of("/\\");
  if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash))
  {
    return "application/octet-stream";
  }
  std::string ext(path.substr(dot + 1));
  for (char &c : ext)
  {
    if (c >= 'A' && c <= 'Z')
    {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  if (ext == "html" || ext == "htm")
    return "text/html; charset=utf-8";
  if (ext == "css")
    return "text/css; charset=utf-8";
  if (ext == "js" || ext == "mjs")
    return "text/javascript; charset=utf-8";
  if (ext == "json")
    return "application/json";
  if (ext == "map")
    return "application/json";
  if (ext == "xml")
    return "application/xml";
  if (ext == "txt")
    return "text/plain; charset=utf-8";
  if (ext == "svg")
    return "image/svg+xml";
  if (ext == "png")
    return "image/png";
  if (ext == "jpg" || ext == "jpeg")
    return "image/jpeg";
  if (ext == "gif")
    return "image/gif";
  if (ext == "webp")
    return "image/webp";
  if (ext == "ico")
    return "image/x-icon";
  if (ext == "wasm")
    return "application/wasm";
  if (ext == "woff")
    return "font/woff";
  if (ext == "woff2")
    return "font/woff2";
  if (ext == "ttf")
    return "font/ttf";
  if (ext == "pdf")
    return "application/pdf";
  if (ext == "csv")
    return "text/csv";
  return "application/octet-stream";
}

handler_t static_file_handler(std::string root, std::string index)
{
  return [root = std::move(root), index = std::move(index)](request_t request) -> vio::task_t<response_t>
  {
    if (request.loop == nullptr)
    {
      co_return response_t::text(status_t::internal_server_error, "no event loop");
    }

    std::string relative;
    if (!safe_relative(percent_decode(request.param("path")), relative))
    {
      co_return response_t::text(status_t::not_found, "Not Found");
    }

    std::string path = root;
    path += '/';
    path += relative.empty() ? index : relative;

    auto info = vio::stat_file(*request.loop, path);
    if (!info.has_value())
    {
      co_return response_t::text(status_t::not_found, "Not Found");
    }
    if ((info->st_mode & S_IFMT) == S_IFDIR)
    {
      if (!path.empty() && path.back() != '/')
      {
        path += '/';
      }
      path += index;
      info = vio::stat_file(*request.loop, path);
      if (!info.has_value())
      {
        co_return response_t::text(status_t::not_found, "Not Found");
      }
    }

    auto file = vio::open_file(*request.loop, path, vio::file_open_flag_t::rdonly, 0);
    if (!file.has_value())
    {
      co_return response_t::text(status_t::not_found, "Not Found");
    }

    auto size = static_cast<std::size_t>(info->st_size);
    std::string body(size, '\0');
    std::size_t offset = 0;
    while (offset < size)
    {
      auto read = co_await vio::read_file(*request.loop, *file.value(), reinterpret_cast<std::uint8_t *>(body.data()) + offset, size - offset, static_cast<std::int64_t>(offset));
      if (!read.has_value())
      {
        co_return response_t::text(status_t::internal_server_error, "read error");
      }
      if (*read == 0)
      {
        break;
      }
      offset += *read;
    }
    body.resize(offset);

    co_return response_t::finished(status_t::ok, std::string(content_type_for_path(path)), std::move(body));
  };
}
} // namespace prism
