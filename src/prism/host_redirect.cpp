#include "host_redirect.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "http.h"
#include "status.h"

namespace prism
{
namespace
{
using table_t = std::vector<std::pair<std::string, std::string>>;

char lower(char c)
{
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string strip_port_lower(std::string host)
{
  if (auto colon = host.find(':'); colon != std::string::npos)
    host.erase(colon);
  for (char &c : host)
    c = lower(c);
  return host;
}

vio::task_t<response_t> redirect_or_forward(std::string scheme, std::shared_ptr<const table_t> table, handler_t inner, request_t request)
{
  const std::string *host_header = request.headers.find("Host");
  const std::string host = strip_port_lower(host_header != nullptr ? *host_header : std::string());
  for (const auto &[from, to] : *table)
  {
    if (host == from)
    {
      response_t response;
      response.status = status_t::permanent_redirect;
      response.headers.set("Location", scheme + "://" + to + request.target);
      co_return response;
    }
  }
  co_return co_await inner(std::move(request));
}
} // namespace

host_redirect_t::host_redirect_t(std::string scheme)
  : _scheme(std::move(scheme))
{
}

void host_redirect_t::add(std::string from_host, std::string to_host)
{
  _table->emplace_back(strip_port_lower(std::move(from_host)), std::move(to_host));
}

bool host_redirect_t::empty() const
{
  return _table->empty();
}

handler_t host_redirect_t::wrap(handler_t inner) const
{
  return std::bind_front(&redirect_or_forward, _scheme, std::shared_ptr<const table_t>(_table), std::move(inner));
}
} // namespace prism
