#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include <prism/app.h>
#include <prism/detail/server.h>
#include <prism/router.h>
#include <prism/server_options.h>

#include <vio/cancellation.h>
#include <vio/operation/tcp.h>
#include <vio/operation/tcp_server.h>
#include <vio/run.h>
#include <vio/task.h>

namespace
{
vio::task_t<void> send_and_read(vio::event_loop_t &loop, int port, std::string request, std::string &response)
{
  auto client_or_error = vio::tcp_create(loop);
  if (!client_or_error.has_value())
  {
    co_return;
  }
  auto client = std::move(client_or_error.value());
  auto addr = vio::ip4_addr("127.0.0.1", port);
  if (!addr.has_value())
  {
    co_return;
  }
  auto connect = co_await vio::tcp_connect(client, reinterpret_cast<const sockaddr *>(&addr.value()));
  if (!connect.has_value())
  {
    co_return;
  }
  auto write_result = co_await vio::write_tcp(client, reinterpret_cast<const uint8_t *>(request.data()), request.size());
  if (!write_result.has_value())
  {
    co_return;
  }
  auto reader_or_error = vio::tcp_create_reader(client);
  if (!reader_or_error.has_value())
  {
    co_return;
  }
  auto reader = std::move(reader_or_error.value());
  for (;;)
  {
    auto read = co_await reader;
    if (!read.has_value())
    {
      break;
    }
    response.append(read.value()->base, read.value()->len);
  }
}

vio::task_t<std::string> run_stream_case(vio::event_loop_t &loop, prism::server_options_t options, std::string request)
{
  prism::app_t app;
  // read_all path
  app.post_stream("/all",
                  [](prism::request_t request) -> vio::task_t<prism::response_t>
                  {
                    std::string body = co_await request.body_stream().read_all();
                    co_return prism::response_t::text(prism::status_t::ok, "all=" + std::to_string(body.size()));
                  });
  // read_into (zero-copy) path
  app.post_stream("/into",
                  [](prism::request_t request) -> vio::task_t<prism::response_t>
                  {
                    std::array<std::byte, 8192> buffer{};
                    std::uint64_t total = 0;
                    prism::request_body_t &body = request.body_stream();
                    for (;;)
                    {
                      std::size_t n = co_await body.read_into(std::span<std::byte>(buffer.data(), buffer.size()));
                      if (n == 0)
                      {
                        break;
                      }
                      total += n;
                    }
                    if (body.status() != prism::body_read_status_t::end)
                    {
                      co_return prism::response_t::text(prism::status_t::bad_request, "interrupted");
                    }
                    co_return prism::response_t::text(prism::status_t::ok, "into=" + std::to_string(total));
                  });

  auto addr = vio::ip4_addr("127.0.0.1", 0);
  REQUIRE(addr.has_value());
  auto server = vio::tcp_create_server(loop);
  REQUIRE(server.has_value());
  auto bound = vio::tcp_bind(server.value(), reinterpret_cast<const sockaddr *>(&addr.value()));
  REQUIRE(bound.has_value());
  auto bound_name = vio::sockname(server->tcp);
  REQUIRE(bound_name.has_value());
  int port = ntohs(reinterpret_cast<const sockaddr_in *>(&bound_name.value())->sin_port);

  vio::cancellation_t cancel;
  auto server_task = prism::detail::serve(std::move(server.value()), std::make_shared<const prism::router_t>(app.router()), nullptr, &cancel, options);

  std::string response;
  co_await send_and_read(loop, port, std::move(request), response);

  cancel.cancel();
  auto serve_result = co_await std::move(server_task);
  CHECK(serve_result.has_value());
  co_return response;
}

std::string post(std::string_view path, std::size_t body_size)
{
  std::string body(body_size, 'x');
  std::string out = "POST ";
  out += path;
  out += " HTTP/1.1\r\nHost: localhost\r\nContent-Length: ";
  out += std::to_string(body_size);
  out += "\r\nConnection: close\r\n\r\n";
  out += body;
  return out;
}
} // namespace

TEST_SUITE("http1 streaming request bodies")
{
  TEST_CASE("read_all drains a large streamed body")
  {
    std::string response;
    vio::run(
      [&](vio::event_loop_t &loop) -> vio::task_t<int>
      {
        response = co_await run_stream_case(loop, {}, post("/all", 1u * 1024u * 1024u));
        co_return 0;
      });
    CHECK(response.find(" 200 ") != std::string::npos);
    CHECK(response.find("all=1048576") != std::string::npos);
  }

  TEST_CASE("read_into (zero-copy) drains a large streamed body")
  {
    std::string response;
    vio::run(
      [&](vio::event_loop_t &loop) -> vio::task_t<int>
      {
        response = co_await run_stream_case(loop, {}, post("/into", 1u * 1024u * 1024u));
        co_return 0;
      });
    CHECK(response.find(" 200 ") != std::string::npos);
    CHECK(response.find("into=1048576") != std::string::npos);
  }

  TEST_CASE("empty streamed body reports end immediately")
  {
    std::string response;
    vio::run(
      [&](vio::event_loop_t &loop) -> vio::task_t<int>
      {
        response = co_await run_stream_case(loop, {}, post("/all", 0));
        co_return 0;
      });
    CHECK(response.find("all=0") != std::string::npos);
  }

  TEST_CASE("configurable header cap rejects oversized headers with 431")
  {
    std::string response;
    vio::run(
      [&](vio::event_loop_t &loop) -> vio::task_t<int>
      {
        prism::server_options_t options;
        options.max_header_bytes = 256;
        std::string request = "POST /all HTTP/1.1\r\nHost: localhost\r\nX-Big: ";
        request += std::string(4096, 'a');
        request += "\r\nContent-Length: 0\r\n\r\n";
        response = co_await run_stream_case(loop, options, std::move(request));
        co_return 0;
      });
    CHECK(response.find(" 431 ") != std::string::npos);
  }
}
