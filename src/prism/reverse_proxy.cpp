#include "reverse_proxy.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vio/operation/http_client.h>

#include "detail/websocket/client.h"
#include "websocket.h"

namespace prism
{
namespace
{
using table_t = std::vector<std::pair<std::string, backend_t>>;

char lower(char c)
{
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool iequals(std::string_view a, std::string_view b)
{
  if (a.size() != b.size())
    return false;
  for (std::size_t i = 0; i < a.size(); ++i)
    if (lower(a[i]) != lower(b[i]))
      return false;
  return true;
}

bool listed_in_connection(std::string_view name, std::string_view connection_value)
{
  std::size_t i = 0;
  while (i < connection_value.size())
  {
    std::size_t start = i;
    while (i < connection_value.size() && connection_value[i] != ',')
      ++i;
    std::string_view token = connection_value.substr(start, i - start);
    while (!token.empty() && (token.front() == ' ' || token.front() == '\t'))
      token.remove_prefix(1);
    while (!token.empty() && (token.back() == ' ' || token.back() == '\t'))
      token.remove_suffix(1);
    if (!token.empty() && iequals(token, name))
      return true;
    if (i < connection_value.size())
      ++i;
  }
  return false;
}

bool base_hop_by_hop(std::string_view name)
{
  static constexpr std::string_view names[] = {"connection", "keep-alive", "proxy-authenticate", "proxy-authorization", "te", "trailer", "transfer-encoding", "upgrade"};
  for (std::string_view n : names)
    if (iequals(name, n))
      return true;
  return false;
}

std::string strip_port_lower(std::string host)
{
  if (auto colon = host.find(':'); colon != std::string::npos)
    host.erase(colon);
  for (char &c : host)
    c = lower(c);
  return host;
}

vio::task_t<response_t> proxy_forward(std::shared_ptr<const table_t> table, request_t request)
{
  const std::string *host_header = request.headers.find("Host");
  const std::string original_host = host_header ? *host_header : std::string();
  const std::string host = strip_port_lower(original_host);

  const backend_t *backend = nullptr;
  for (const auto &entry : *table)
  {
    if (entry.first == host)
    {
      backend = &entry.second;
      break;
    }
  }
  if (backend == nullptr)
    co_return response_t::text(static_cast<status_t>(502), "reverse_proxy: no route for host");

  const std::string *req_connection = request.headers.find("Connection");
  const std::string_view req_conn_value = req_connection ? std::string_view(*req_connection) : std::string_view();

  vio::http::request_t out;
  out.method = std::string(method_name(request.method));
  out.url = "http://" + backend->host + ":" + std::to_string(backend->port) + request.target;
  out.allow_plaintext = true;
  out.body = request.body;
  for (const header_t &h : request.headers.entries)
  {
    if (base_hop_by_hop(h.name) || listed_in_connection(h.name, req_conn_value) || iequals(h.name, "host") || iequals(h.name, "content-length") || iequals(h.name, "user-agent") ||
        iequals(h.name, "accept-encoding") || iequals(h.name, "x-forwarded-for"))
      continue;
    out.headers.push_back(vio::http::header_t{h.name, h.value});
  }
  out.headers.push_back(vio::http::header_t{"X-Forwarded-Proto", "https"});
  if (!request.client_ip.empty())
    out.headers.push_back(vio::http::header_t{"X-Forwarded-For", request.client_ip});
  if (!original_host.empty())
    out.headers.push_back(vio::http::header_t{"X-Forwarded-Host", original_host});

  auto upstream = co_await vio::http::fetch_once(*request.loop, out);
  if (!upstream)
    co_return response_t::text(static_cast<status_t>(502), "reverse_proxy: upstream error");

  const std::string_view resp_conn_value = upstream->header("Connection");
  response_t result;
  result.status = static_cast<status_t>(upstream->status);
  for (const vio::http::header_t &h : upstream->headers)
  {
    if (base_hop_by_hop(h.name) || listed_in_connection(h.name, resp_conn_value))
      continue;
    result.headers.entries.push_back(header_t{h.name, h.value});
  }
  result.body = std::move(upstream->body);
  co_return result;
}

vio::task_t<void> relay_direction(std::shared_ptr<ws_connection_t> from, std::shared_ptr<ws_connection_t> to)
{
  for (;;)
  {
    ws_message_t message = co_await from->receive();
    if (!message.ok)
    {
      break;
    }
    bool ok = false;
    if (message.is_text)
    {
      ok = co_await to->send_text(message.data);
    }
    else
    {
      ok = co_await to->send_binary(std::as_bytes(std::span<const char>(message.data.data(), message.data.size())));
    }
    if (!ok)
    {
      break;
    }
  }
  to->close(1000, "");
  co_return;
}

vio::task_t<void> proxy_websocket(std::shared_ptr<const table_t> table, std::shared_ptr<ws_connection_t> client)
{
  const request_t &request = client->request();
  const std::string *host_header = request.headers.find("Host");
  const std::string host = strip_port_lower(host_header ? *host_header : std::string());

  const backend_t *backend = nullptr;
  for (const auto &entry : *table)
  {
    if (entry.first == host)
    {
      backend = &entry.second;
      break;
    }
  }
  if (backend == nullptr || request.loop == nullptr)
  {
    client->close(1011, "reverse_proxy: no route for host");
    co_return;
  }

  const std::string *req_connection = request.headers.find("Connection");
  const std::string_view req_conn_value = req_connection ? std::string_view(*req_connection) : std::string_view();

  std::vector<header_t> forwarded;
  for (const header_t &h : request.headers.entries)
  {
    if (base_hop_by_hop(h.name) || listed_in_connection(h.name, req_conn_value) || iequals(h.name, "host") || iequals(h.name, "sec-websocket-key") || iequals(h.name, "sec-websocket-version") ||
        iequals(h.name, "sec-websocket-accept") || iequals(h.name, "sec-websocket-extensions") || iequals(h.name, "x-forwarded-for"))
    {
      continue;
    }
    forwarded.push_back(header_t{h.name, h.value});
  }
  forwarded.push_back(header_t{"Host", backend->host});
  forwarded.push_back(header_t{"X-Forwarded-Proto", "https"});
  if (!request.client_ip.empty())
  {
    forwarded.push_back(header_t{"X-Forwarded-For", request.client_ip});
  }
  if (!host.empty())
  {
    forwarded.push_back(header_t{"X-Forwarded-Host", host});
  }

  auto upstream = co_await detail::websocket::connect_client(*request.loop, backend->host, backend->port, request.target, std::move(forwarded), server_options_t{});
  if (!upstream)
  {
    client->close(1011, "reverse_proxy: upstream websocket error");
    co_return;
  }

  auto client_to_upstream = relay_direction(client, upstream);
  co_await relay_direction(upstream, client);
  co_await std::move(client_to_upstream);
  co_return;
}
} // namespace

void reverse_proxy_t::add_route(std::string host, backend_t backend)
{
  _table->emplace_back(strip_port_lower(std::move(host)), std::move(backend));
}

handler_t reverse_proxy_t::handler() const
{
  return std::bind_front(&proxy_forward, _table);
}

ws_handler_t reverse_proxy_t::ws_handler() const
{
  return std::bind_front(&proxy_websocket, _table);
}

void reverse_proxy_t::install(app_t &app) const
{
  app.ws("/{path...}", ws_handler());
  app.any("/{path...}", handler());
}
} // namespace prism
