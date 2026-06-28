#include <doctest/doctest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
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

vio::task_t<std::string> run_case(vio::event_loop_t &loop, prism::keepalive_options_t options, std::string request)
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

  vio::cancellation_t cancel;
  auto server_task = prism::detail::serve(std::move(server.value()), std::make_shared<const prism::router_t>(app.router()), &cancel, options);

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

  bool got = response.find("pong") != std::string::npos;
  co_await vio::sleep(loop, hold);
  co_return got;
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

TEST_CASE("keep-alive: server forces Connection: close once max_requests is reached")
{
  int rc = vio::run(
    [](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      prism::keepalive_options_t options;
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
      prism::keepalive_options_t options;
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
      prism::keepalive_options_t options;
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

      prism::keepalive_options_t options;
      options.write_timeout = std::chrono::seconds{5};
      vio::cancellation_t cancel;
      auto server_task = prism::detail::serve(std::move(server.value()), std::make_shared<const prism::router_t>(app.router()), &cancel, options);

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
      prism::keepalive_options_t options;
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
      prism::keepalive_options_t options;
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

      prism::keepalive_options_t options;
      options.max_connections = 1;
      options.idle_timeout = std::chrono::seconds{30};
      vio::cancellation_t cancel;
      auto server_task = prism::detail::serve(std::move(server.value()), std::make_shared<const prism::router_t>(app.router()), &cancel, options);

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
