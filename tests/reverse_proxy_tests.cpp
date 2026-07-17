#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include <prism/http.h>
#include <prism/reverse_proxy.h>

#include <vio/operation/tcp.h>
#include <vio/operation/tcp_server.h>
#include <vio/run.h>
#include <vio/task.h>

namespace
{
std::size_t count_occurrences(const std::string &haystack, std::string_view needle)
{
  std::size_t count = 0;
  std::size_t pos = 0;
  while ((pos = haystack.find(needle, pos)) != std::string::npos)
  {
    ++count;
    pos += needle.size();
  }
  return count;
}

std::size_t body_length(const std::string &request)
{
  auto pos = request.find("Content-Length: ");
  if (pos == std::string::npos)
    return 0;
  pos += 16;
  std::size_t n = 0;
  while (pos < request.size() && request[pos] >= '0' && request[pos] <= '9')
  {
    n = n * 10 + static_cast<std::size_t>(request[pos] - '0');
    ++pos;
  }
  return n;
}

vio::task_t<void> backend_serve(vio::tcp_server_t server, std::string &captured)
{
  auto listener = std::move(server);
  auto listen_result = co_await vio::tcp_listen(listener, 10);
  if (!listen_result.has_value())
    co_return;
  auto accepted = vio::tcp_accept(listener);
  if (!accepted.has_value())
    co_return;
  auto conn = std::move(accepted.value());

  auto reader_or_error = vio::tcp_create_reader(conn);
  if (!reader_or_error.has_value())
    co_return;
  auto reader = std::move(reader_or_error.value());
  for (;;)
  {
    auto header_end = captured.find("\r\n\r\n");
    if (header_end != std::string::npos && captured.size() - (header_end + 4) >= body_length(captured))
      break;
    auto chunk = co_await reader;
    if (!chunk.has_value())
      break;
    captured.append(chunk.value().buf.base, chunk.value().buf.len);
  }

  const std::string method = captured.substr(0, captured.find(' '));
  const std::string body = (method == "HEAD") ? std::string() : std::string("backend-body");
  std::string response = "HTTP/1.1 200 OK\r\n";
  response += "X-Seen-Method: " + method + "\r\n";
  response += "Keep-Alive: timeout=5\r\n";
  response += "Content-Length: 12\r\n";
  response += "\r\n";
  response += body;
  (void)co_await vio::write_tcp(conn, reinterpret_cast<const uint8_t *>(response.data()), response.size());
}

vio::task_t<void> run_proxy(vio::event_loop_t &loop, prism::method_t method, std::string body, prism::response_t &out, std::string &captured)
{
  auto addr = vio::ip4_addr("127.0.0.1", 0);
  REQUIRE(addr.has_value());
  auto server = vio::tcp_create_server(loop);
  REQUIRE(server.has_value());
  auto bound = vio::tcp_bind(server.value(), reinterpret_cast<const sockaddr *>(&addr.value()));
  REQUIRE(bound.has_value());
  auto bound_name = vio::sockname(server->tcp);
  REQUIRE(bound_name.has_value());
  const int port = ntohs(reinterpret_cast<const sockaddr_in *>(&bound_name.value())->sin_port);

  auto backend_task = backend_serve(std::move(server.value()), captured);

  prism::reverse_proxy_t proxy;
  proxy.add_route("backend.local", prism::backend_t{"127.0.0.1", static_cast<std::uint16_t>(port)});

  prism::request_t request;
  request.loop = &loop;
  request.method = method;
  request.target = "/app/path?x=1";
  request.body = std::move(body);
  request.headers.entries.push_back(prism::header_t{"Host", "backend.local"});
  request.headers.entries.push_back(prism::header_t{"Connection", "close, X-Custom-Hop"});
  request.headers.entries.push_back(prism::header_t{"X-Custom-Hop", "drop-me"});
  request.headers.entries.push_back(prism::header_t{"Keep-Alive", "timeout=1"});
  request.headers.entries.push_back(prism::header_t{"User-Agent", "test-agent"});
  request.headers.entries.push_back(prism::header_t{"X-Keep", "keep-me"});
  if (!request.body.empty())
    request.headers.entries.push_back(prism::header_t{"Content-Length", std::to_string(request.body.size())});

  out = co_await proxy.handler()(std::move(request));
  co_await std::move(backend_task);
}
} // namespace

TEST_SUITE("reverse_proxy")
{
  TEST_CASE("forwards a GET, strips hop-by-hop headers both ways, single Host, adds X-Forwarded-*")
  {
    const int rc = vio::run(
      [](vio::event_loop_t &loop) -> vio::task_t<int>
      {
        prism::response_t out;
        std::string captured;
        co_await run_proxy(loop, prism::method_t::get, "", out, captured);

        CHECK(static_cast<int>(out.status) == 200);
        CHECK(out.body == "backend-body");

        CHECK(captured.find("GET /app/path?x=1 HTTP/1.1\r\n") != std::string::npos);
        CHECK(count_occurrences(captured, "\r\nHost:") == 1);
        CHECK(captured.find("\r\nHost: 127.0.0.1:") != std::string::npos);
        CHECK(captured.find("X-Forwarded-Proto: https\r\n") != std::string::npos);
        CHECK(captured.find("X-Forwarded-Host: backend.local\r\n") != std::string::npos);
        CHECK(captured.find("X-Keep: keep-me\r\n") != std::string::npos);
        CHECK(captured.find("X-Custom-Hop") == std::string::npos);
        CHECK(captured.find("Keep-Alive") == std::string::npos);
        CHECK(captured.find("test-agent") == std::string::npos);

        CHECK(out.headers.find("X-Seen-Method") != nullptr);
        CHECK(out.headers.find("Keep-Alive") == nullptr);
        co_return 0;
      });
    CHECK(rc == 0);
  }

  TEST_CASE("forwards a POST body")
  {
    const int rc = vio::run(
      [](vio::event_loop_t &loop) -> vio::task_t<int>
      {
        prism::response_t out;
        std::string captured;
        co_await run_proxy(loop, prism::method_t::post, "hello-body", out, captured);

        CHECK(static_cast<int>(out.status) == 200);
        CHECK(out.body == "backend-body");
        CHECK(captured.find("POST /app/path?x=1 HTTP/1.1\r\n") != std::string::npos);
        CHECK(captured.ends_with("hello-body"));
        co_return 0;
      });
    CHECK(rc == 0);
  }

  TEST_CASE("forwards HEAD (no body) and OPTIONS")
  {
    const int rc = vio::run(
      [](vio::event_loop_t &loop) -> vio::task_t<int>
      {
        prism::response_t head_out;
        std::string head_captured;
        co_await run_proxy(loop, prism::method_t::head, "", head_out, head_captured);
        CHECK(static_cast<int>(head_out.status) == 200);
        CHECK(head_out.body.empty());
        CHECK(head_captured.find("HEAD /app/path?x=1 HTTP/1.1\r\n") != std::string::npos);

        prism::response_t options_out;
        std::string options_captured;
        co_await run_proxy(loop, prism::method_t::options, "", options_out, options_captured);
        CHECK(static_cast<int>(options_out.status) == 200);
        CHECK(options_captured.find("OPTIONS /app/path?x=1 HTTP/1.1\r\n") != std::string::npos);
        co_return 0;
      });
    CHECK(rc == 0);
  }

  TEST_CASE("an unmapped Host returns 502 without contacting a backend")
  {
    const int rc = vio::run(
      [](vio::event_loop_t &loop) -> vio::task_t<int>
      {
        prism::reverse_proxy_t proxy;
        proxy.add_route("known.local", prism::backend_t{"127.0.0.1", 9});

        prism::request_t request;
        request.loop = &loop;
        request.method = prism::method_t::get;
        request.target = "/";
        request.headers.entries.push_back(prism::header_t{"Host", "unknown.local"});

        auto out = co_await proxy.handler()(std::move(request));
        CHECK(static_cast<int>(out.status) == 502);
        co_return 0;
      });
    CHECK(rc == 0);
  }
}
