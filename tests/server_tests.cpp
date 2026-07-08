#include <doctest/doctest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <prism/app.h>
#include <prism/detail/server.h>
#include <prism/router.h>
#include <prism/server_options.h>

#include <vio/cancellation.h>
#include <vio/operation/sleep.h>
#include <vio/operation/tcp.h>
#include <vio/operation/tcp_server.h>
#include <vio/run.h>
#include <vio/task.h>

namespace
{
vio::task_t<void> run_client(vio::event_loop_t &loop, int port, std::string request, std::string &response)
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
    auto &buffer = read.value();
    response.append(buffer->base, buffer->len);
  }
}

vio::task_t<void> run_client6(vio::event_loop_t &loop, int port, std::string request, std::string &response)
{
  auto client_or_error = vio::tcp_create(loop);
  if (!client_or_error.has_value())
  {
    co_return;
  }
  auto client = std::move(client_or_error.value());

  auto addr = vio::ip6_addr("::1", port);
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
    auto &buffer = read.value();
    response.append(buffer->base, buffer->len);
  }
}

vio::task_t<std::string> run_case(vio::event_loop_t &loop, prism::server_options_t options, std::string request)
{
  prism::app_t app;
  app.get("/ping",
          [](prism::request_t) -> vio::task_t<prism::response_t>
          {
            co_return prism::response_t::text(prism::status_t::ok, "pong");
          });
  app.get("/stream",
          [](prism::request_t) -> vio::task_t<prism::response_t>
          {
            auto counter = std::make_shared<int>(0);
            co_return prism::response_t::streaming(prism::status_t::ok, "text/plain",
                                                   [counter]() -> vio::task_t<prism::body_chunk_t>
                                                   {
                                                     int n = (*counter)++;
                                                     if (n >= 3)
                                                     {
                                                       co_return prism::body_chunk_t{"", true};
                                                     }
                                                     co_return prism::body_chunk_t{"chunk" + std::to_string(n) + ";", false};
                                                   });
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
  co_await run_client(loop, port, std::move(request), response);

  cancel.cancel();
  auto serve_result = co_await std::move(server_task);
  CHECK(serve_result.has_value());

  co_return response;
}

vio::task_t<bool> hold_then_close(vio::event_loop_t &loop, int port, std::chrono::milliseconds hold)
{
  auto client_or_error = vio::tcp_create(loop);
  if (!client_or_error.has_value())
  {
    co_return false;
  }
  auto client = std::move(client_or_error.value());

  auto addr = vio::ip4_addr("127.0.0.1", port);
  if (!addr.has_value())
  {
    co_return false;
  }
  auto connect = co_await vio::tcp_connect(client, reinterpret_cast<const sockaddr *>(&addr.value()));
  if (!connect.has_value())
  {
    co_return false;
  }

  std::string request = "GET /ping HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
  auto write_result = co_await vio::write_tcp(client, reinterpret_cast<const uint8_t *>(request.data()), request.size());
  if (!write_result.has_value())
  {
    co_return false;
  }

  auto reader_or_error = vio::tcp_create_reader(client);
  if (!reader_or_error.has_value())
  {
    co_return false;
  }
  auto reader = std::move(reader_or_error.value());
  std::string response;
  vio::cancellation_t token;
  auto watchdog = [](vio::event_loop_t &el, vio::tcp_reader_t &rd, vio::cancellation_t &tok, std::chrono::milliseconds dur) -> vio::task_t<void>
  {
    auto fired = co_await vio::sleep(el, dur, &tok);
    if (fired.has_value() && !tok.is_cancelled())
    {
      rd.cancel();
    }
  }(loop, reader, token, std::chrono::seconds{5});

  while (response.find("pong") == std::string::npos)
  {
    auto read = co_await reader;
    if (!read.has_value())
    {
      break;
    }
    auto &buffer = read.value();
    response.append(buffer->base, buffer->len);
  }
  token.cancel();
  co_await std::move(watchdog);

  co_await vio::sleep(loop, hold);
  co_return response.find("pong") != std::string::npos;
}

struct probe_result_t
{
  bool connected = false;
  std::string response;
};

vio::task_t<probe_result_t> probe_client(vio::event_loop_t &loop, int port, std::string request)
{
  probe_result_t result;
  auto client_or_error = vio::tcp_create(loop);
  if (!client_or_error.has_value())
  {
    co_return result;
  }
  auto client = std::move(client_or_error.value());

  auto addr = vio::ip4_addr("127.0.0.1", port);
  if (!addr.has_value())
  {
    co_return result;
  }
  auto connect = co_await vio::tcp_connect(client, reinterpret_cast<const sockaddr *>(&addr.value()));
  if (!connect.has_value())
  {
    co_return result;
  }
  result.connected = true;

  auto write_result = co_await vio::write_tcp(client, reinterpret_cast<const uint8_t *>(request.data()), request.size());
  if (!write_result.has_value())
  {
    co_return result;
  }

  auto reader_or_error = vio::tcp_create_reader(client);
  if (!reader_or_error.has_value())
  {
    co_return result;
  }
  auto reader = std::move(reader_or_error.value());
  for (;;)
  {
    auto read = co_await reader;
    if (!read.has_value())
    {
      break;
    }
    auto &buffer = read.value();
    result.response.append(buffer->base, buffer->len);
  }
  co_return result;
}
} // namespace

TEST_CASE("app serves an HTTP/1.1 request end to end over TCP")
{
  int rc = vio::run(
    [](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      std::string response = co_await run_case(loop, {}, "GET /ping HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
      CHECK(response.find("HTTP/1.1 200 OK") != std::string::npos);
      CHECK(response.find("Content-Length: 4\r\n") != std::string::npos);
      CHECK(response.find("Connection: close\r\n") != std::string::npos);
      CHECK(response.ends_with("pong"));
      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("app streams a chunked HTTP/1.1 response")
{
  int rc = vio::run(
    [](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      std::string response = co_await run_case(loop, {}, "GET /stream HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
      CHECK(response.find("HTTP/1.1 200 OK") != std::string::npos);
      CHECK(response.find("Transfer-Encoding: chunked\r\n") != std::string::npos);
      CHECK(response.find("Content-Length:") == std::string::npos);
      CHECK(response.find("7\r\nchunk0;\r\n") != std::string::npos);
      CHECK(response.find("7\r\nchunk2;\r\n") != std::string::npos);
      CHECK(response.ends_with("0\r\n\r\n"));
      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("keep-alive: server forces Connection: close once max_requests is reached")
{
  int rc = vio::run(
    [](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      prism::server_options_t options;
      options.max_requests = 1;
      std::string response = co_await run_case(loop, options, "GET /ping HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n");
      CHECK(response.find("HTTP/1.1 200 OK") != std::string::npos);
      CHECK(response.find("Connection: close\r\n") != std::string::npos);
      CHECK(response.find("Connection: keep-alive") == std::string::npos);
      CHECK(response.ends_with("pong"));
      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("keep-alive: an idle connection is closed after the idle timeout")
{
  int rc = vio::run(
    [](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      prism::server_options_t options;
      options.idle_timeout = std::chrono::milliseconds{100};
      options.header_timeout = std::chrono::seconds{30};
      options.body_timeout = std::chrono::seconds{30};
      std::string response = co_await run_case(loop, options, "GET /ping HTTP/1.1\r\nHost: localhost\r\n\r\n");
      CHECK(response.find("HTTP/1.1 200 OK") != std::string::npos);
      CHECK(response.ends_with("pong"));
      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("keep-alive: a slow partial request is closed after the request timeout")
{
  int rc = vio::run(
    [](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      prism::server_options_t options;
      options.idle_timeout = std::chrono::seconds{30};
      options.header_timeout = std::chrono::milliseconds{100};
      std::string response = co_await run_case(loop, options, "GET /ping HTTP/1.1\r\nHost: localhost\r\n");
      CHECK(response.empty());
      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("a large response is delivered through the write-timeout path")
{
  int rc = vio::run(
    [](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      constexpr std::size_t big_size = 4u * 1024u * 1024u;
      prism::app_t app;
      app.get("/big",
              [](prism::request_t) -> vio::task_t<prism::response_t>
              {
                co_return prism::response_t::text(prism::status_t::ok, std::string(4u * 1024u * 1024u, 'x'));
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

      prism::server_options_t options;
      options.write_timeout = std::chrono::seconds{5};
      vio::cancellation_t cancel;
      auto server_task = prism::detail::serve(std::move(server.value()), std::make_shared<const prism::router_t>(app.router()), nullptr, &cancel, options);

      std::string response;
      co_await run_client(loop, port, "GET /big HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", response);

      cancel.cancel();
      auto serve_result = co_await std::move(server_task);
      CHECK(serve_result.has_value());

      CHECK(response.find("HTTP/1.1 200 OK") != std::string::npos);
      CHECK(response.find("Content-Length: 4194304\r\n") != std::string::npos);
      CHECK(response.size() >= big_size);

      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("keep-alive: a stalled request body is closed after the body timeout")
{
  int rc = vio::run(
    [](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      prism::server_options_t options;
      options.idle_timeout = std::chrono::seconds{3};
      options.header_timeout = std::chrono::seconds{3};
      options.body_timeout = std::chrono::milliseconds{100};
      auto start = std::chrono::steady_clock::now();
      std::string response = co_await run_case(loop, options, "POST /ping HTTP/1.1\r\nHost: localhost\r\nContent-Length: 100\r\n\r\n");
      auto elapsed = std::chrono::steady_clock::now() - start;
      CHECK(response.empty());
      CHECK(elapsed < std::chrono::seconds{1});
      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("keep-alive: a pipelined request with a stalled body honors body_timeout, not idle_timeout")
{
  int rc = vio::run(
    [](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      prism::server_options_t options;
      options.idle_timeout = std::chrono::seconds{10};
      options.header_timeout = std::chrono::seconds{10};
      options.body_timeout = std::chrono::milliseconds{100};
      auto start = std::chrono::steady_clock::now();
      std::string response = co_await run_case(loop, options,
                                               "GET /ping HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n"
                                               "POST /ping HTTP/1.1\r\nHost: localhost\r\nContent-Length: 100\r\n\r\n");
      auto elapsed = std::chrono::steady_clock::now() - start;
      CHECK(response.find("pong") != std::string::npos);
      CHECK(elapsed < std::chrono::seconds{3});
      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("max_connections sheds connections beyond the cap")
{
  int rc = vio::run(
    [](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      prism::app_t app;
      app.get("/ping",
              [](prism::request_t) -> vio::task_t<prism::response_t>
              {
                co_return prism::response_t::text(prism::status_t::ok, "pong");
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

      prism::server_options_t options;
      options.max_connections = 1;
      options.idle_timeout = std::chrono::seconds{30};
      vio::cancellation_t cancel;
      auto server_task = prism::detail::serve(std::move(server.value()), std::make_shared<const prism::router_t>(app.router()), nullptr, &cancel, options);

      auto holder = hold_then_close(loop, port, std::chrono::milliseconds{400});

      co_await vio::sleep(loop, std::chrono::milliseconds{150});
      auto shed = co_await probe_client(loop, port, "GET /ping HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");

      bool first_ok = co_await std::move(holder);

      cancel.cancel();
      auto serve_result = co_await std::move(server_task);
      CHECK(serve_result.has_value());

      CHECK(first_ok);
      CHECK(shed.connected);
      CHECK(shed.response.empty());

      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("vio::write_tcp takes ownership of a moved std::vector on the cancellable path")
{
  int rc = vio::run(
    [](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      auto addr = vio::ip4_addr("127.0.0.1", 0);
      REQUIRE(addr.has_value());
      auto server = vio::tcp_create_server(loop);
      REQUIRE(server.has_value());
      auto bound = vio::tcp_bind(server.value(), reinterpret_cast<const sockaddr *>(&addr.value()));
      REQUIRE(bound.has_value());

      auto bound_name = vio::sockname(server->tcp);
      REQUIRE(bound_name.has_value());
      int port = ntohs(reinterpret_cast<const sockaddr_in *>(&bound_name.value())->sin_port);

      auto received = std::make_shared<std::string>();
      auto accept_task = [](vio::tcp_server_t srv, std::shared_ptr<std::string> out) -> vio::task_t<void>
      {
        auto listen = co_await vio::tcp_listen(srv, 128, nullptr);
        if (!listen.has_value())
        {
          co_return;
        }
        auto client = vio::tcp_accept(srv);
        if (!client.has_value())
        {
          co_return;
        }
        auto reader_or_error = vio::tcp_create_reader(client.value());
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
          out->append(read.value()->base, read.value()->len);
        }
      }(std::move(server.value()), received);

      std::vector<std::uint8_t> payload(4096);
      for (std::size_t i = 0; i < payload.size(); ++i)
      {
        payload[i] = static_cast<std::uint8_t>(i);
      }
      std::string expected(payload.begin(), payload.end());

      bool delivered = co_await [](vio::event_loop_t &el, int p, std::vector<std::uint8_t> bytes) -> vio::task_t<bool>
      {
        auto client_or_error = vio::tcp_create(el);
        if (!client_or_error.has_value())
        {
          co_return false;
        }
        auto client = std::move(client_or_error.value());
        auto a = vio::ip4_addr("127.0.0.1", p);
        if (!a.has_value())
        {
          co_return false;
        }
        auto connect = co_await vio::tcp_connect(client, reinterpret_cast<const sockaddr *>(&a.value()));
        if (!connect.has_value())
        {
          co_return false;
        }
        vio::cancellation_t token;
        auto written = co_await vio::write_tcp(client, std::move(bytes), &token);
        co_return written.has_value();
      }(loop, port, std::move(payload));

      co_await std::move(accept_task);

      CHECK(delivered);
      CHECK(*received == expected);

      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("the server emits an access log line per request through the logger")
{
  int rc = vio::run(
    [](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      prism::app_t app;
      app.get("/ping",
              [](prism::request_t) -> vio::task_t<prism::response_t>
              {
                co_return prism::response_t::text(prism::status_t::ok, "pong");
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

      auto lines = std::make_shared<std::vector<std::string>>();
      auto logger = std::make_shared<prism::logger_t>();
      logger->set_sink(
        [lines](prism::log_level_t level, std::string_view message)
        {
          if (level == prism::log_level_t::info)
          {
            lines->emplace_back(message);
          }
        });

      vio::cancellation_t cancel;
      auto server_task = prism::detail::serve(std::move(server.value()), std::make_shared<const prism::router_t>(app.router()), logger, &cancel, prism::server_options_t{});

      std::string response;
      co_await run_client(loop, port, "GET /ping HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", response);

      cancel.cancel();
      auto serve_result = co_await std::move(server_task);
      CHECK(serve_result.has_value());

      bool found = false;
      for (const auto &line : *lines)
      {
        if (line.rfind("GET /ping 200", 0) == 0)
        {
          found = true;
        }
      }
      CHECK(found);

      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("a handler can co_await async work via the event loop on request.loop")
{
  int rc = vio::run(
    [](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      prism::app_t app;
      app.get("/wait",
              [](prism::request_t request) -> vio::task_t<prism::response_t>
              {
                co_await vio::sleep(*request.loop, std::chrono::milliseconds{20});
                co_return prism::response_t::text(prism::status_t::ok, "waited");
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
      auto server_task = prism::detail::serve(std::move(server.value()), std::make_shared<const prism::router_t>(app.router()), nullptr, &cancel, prism::server_options_t{});

      std::string response;
      co_await run_client(loop, port, "GET /wait HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", response);

      cancel.cancel();
      auto serve_result = co_await std::move(server_task);
      CHECK(serve_result.has_value());

      CHECK(response.find("HTTP/1.1 200 OK") != std::string::npos);
      CHECK(response.ends_with("waited"));

      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("a dual-stack listener on :: accepts both IPv4 and IPv6 loopback clients")
{
  int rc = vio::run(
    [](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      prism::app_t app;
      app.get("/ping",
              [](prism::request_t) -> vio::task_t<prism::response_t>
              {
                co_return prism::response_t::text(prism::status_t::ok, "pong");
              });

      auto addr = vio::ip6_addr("::", 0);
      REQUIRE(addr.has_value());
      auto server = vio::tcp_create_server(loop);
      REQUIRE(server.has_value());
      auto bound = vio::tcp_bind(server.value(), reinterpret_cast<const sockaddr *>(&addr.value()));
      if (!bound.has_value())
      {
        co_return 0;
      }

      auto bound_name = vio::sockname(server->tcp);
      REQUIRE(bound_name.has_value());
      int port = ntohs(reinterpret_cast<const sockaddr_in6 *>(&bound_name.value())->sin6_port);

      vio::cancellation_t cancel;
      auto server_task = prism::detail::serve(std::move(server.value()), std::make_shared<const prism::router_t>(app.router()), nullptr, &cancel, prism::server_options_t{});

      std::string response4;
      co_await run_client(loop, port, "GET /ping HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", response4);

      std::string response6;
      co_await run_client6(loop, port, "GET /ping HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", response6);

      cancel.cancel();
      auto serve_result = co_await std::move(server_task);
      CHECK(serve_result.has_value());

      CHECK(response4.find("pong") != std::string::npos);
      if (!response6.empty())
      {
        CHECK(response6.find("pong") != std::string::npos);
      }

      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("query parameters survive the codec and reach the handler")
{
  int rc = vio::run(
    [](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      prism::app_t app;
      app.get("/echo",
              [](prism::request_t request) -> vio::task_t<prism::response_t>
              {
                co_return prism::response_t::text(prism::status_t::ok, request.query("name"));
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
      auto server_task = prism::detail::serve(std::move(server.value()), std::make_shared<const prism::router_t>(app.router()), nullptr, &cancel, prism::server_options_t{});

      std::string response;
      co_await run_client(loop, port, "GET /echo?name=ada+lovelace HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", response);

      cancel.cancel();
      auto serve_result = co_await std::move(server_task);
      CHECK(serve_result.has_value());

      CHECK(response.ends_with("ada lovelace"));

      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("multi-worker: reuseport workers serve concurrent requests and drain cleanly on cancel")
{
  int rc = vio::run(
    [](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      prism::app_t app;
      app.get("/ping",
              [](prism::request_t) -> vio::task_t<prism::response_t>
              {
                co_return prism::response_t::text(prism::status_t::ok, "pong");
              });

      prism::server_options_t options;
      options.worker_threads = 4;
      options.shutdown_timeout = std::chrono::seconds{5};

      const int port = 39517;
      vio::cancellation_t cancel;
      auto listen_task = app.listen(loop, "127.0.0.1", static_cast<uint16_t>(port), &cancel, options);

      co_await vio::sleep(loop, std::chrono::milliseconds{150}, nullptr);

      constexpr int request_count = 24;
      std::vector<std::string> responses(request_count);
      std::vector<vio::task_t<void>> clients;
      clients.reserve(request_count);
      for (int i = 0; i < request_count; ++i)
      {
        clients.push_back(run_client(loop, port, "GET /ping HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", responses[static_cast<std::size_t>(i)]));
      }
      for (auto &client : clients)
      {
        co_await std::move(client);
      }

      int ok = 0;
      for (const auto &response : responses)
      {
        if (response.find("HTTP/1.1 200 OK") != std::string::npos && response.ends_with("pong"))
        {
          ++ok;
        }
      }
      CHECK(ok == request_count);

      cancel.cancel();
      auto result = co_await std::move(listen_task);
      CHECK(result.has_value());

      co_return 0;
    });
  CHECK(rc == 0);
}
