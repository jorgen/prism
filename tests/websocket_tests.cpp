#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <prism/app.h>
#include <prism/detail/server.h>
#include <prism/detail/websocket/frame.h>
#include <prism/detail/websocket/handshake.h>
#include <prism/router.h>
#include <prism/server_options.h>
#include <prism/websocket.h>

#include <vio/cancellation.h>
#include <vio/operation/tcp.h>
#include <vio/operation/tcp_server.h>
#include <vio/run.h>
#include <vio/task.h>

namespace ws = prism::detail::websocket;

namespace
{
std::string bytes(std::initializer_list<int> values)
{
  std::string out;
  for (int value : values)
  {
    out.push_back(static_cast<char>(static_cast<unsigned char>(value)));
  }
  return out;
}

std::string mask_frame(std::uint8_t opcode, std::string_view payload, bool fin = true)
{
  std::string out;
  out.push_back(static_cast<char>((fin ? 0x80 : 0x00) | opcode));
  const std::uint8_t key[4] = {0x12, 0x34, 0x56, 0x78};
  out.push_back(static_cast<char>(0x80 | static_cast<std::uint8_t>(payload.size())));
  for (unsigned char k : key)
  {
    out.push_back(static_cast<char>(k));
  }
  for (std::size_t i = 0; i < payload.size(); ++i)
  {
    out.push_back(static_cast<char>(static_cast<std::uint8_t>(payload[i]) ^ key[i & 3]));
  }
  return out;
}

struct server_frame_t
{
  bool ok = false;
  std::uint8_t opcode = 0;
  bool fin = true;
  std::string payload;
  std::size_t consumed = 0;
};

server_frame_t parse_server_frame(std::string_view buffer)
{
  if (buffer.size() < 2)
  {
    return {};
  }
  const auto b0 = static_cast<std::uint8_t>(buffer[0]);
  const auto b1 = static_cast<std::uint8_t>(buffer[1]);
  server_frame_t frame;
  frame.fin = (b0 & 0x80) != 0;
  frame.opcode = b0 & 0x0f;
  const bool masked = (b1 & 0x80) != 0;
  std::size_t len = b1 & 0x7f;
  std::size_t offset = 2;
  if (len == 126)
  {
    if (buffer.size() < 4)
    {
      return {};
    }
    len = (static_cast<std::size_t>(static_cast<std::uint8_t>(buffer[2])) << 8) | static_cast<std::uint8_t>(buffer[3]);
    offset = 4;
  }
  if (masked)
  {
    offset += 4;
  }
  if (buffer.size() < offset + len)
  {
    return {};
  }
  frame.ok = true;
  frame.payload = std::string(buffer.substr(offset, len));
  frame.consumed = offset + len;
  return frame;
}

struct ws_exchange_t
{
  std::string handshake;
  std::string echo_payload;
  std::uint8_t echo_opcode = 0;
  std::uint8_t close_opcode = 0;
};

vio::task_t<ws_exchange_t> run_ws_client(vio::event_loop_t &loop, int port, std::string path, std::string send_frames)
{
  ws_exchange_t result;
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

  std::string upgrade = "GET " + path +
                        " HTTP/1.1\r\n"
                        "Host: localhost\r\n"
                        "Upgrade: websocket\r\n"
                        "Connection: Upgrade\r\n"
                        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                        "Sec-WebSocket-Version: 13\r\n\r\n";
  auto wrote = co_await vio::write_tcp(client, reinterpret_cast<const uint8_t *>(upgrade.data()), upgrade.size());
  if (!wrote.has_value())
  {
    co_return result;
  }

  auto reader_or_error = vio::tcp_create_reader(client);
  if (!reader_or_error.has_value())
  {
    co_return result;
  }
  auto reader = std::move(reader_or_error.value());
  std::string buffer;

  auto pull = [&]() -> vio::task_t<bool>
  {
    auto read = co_await reader;
    if (!read.has_value())
    {
      co_return false;
    }
    buffer.append(read.value()->base, read.value()->len);
    co_return true;
  };

  while (buffer.find("\r\n\r\n") == std::string::npos)
  {
    if (!co_await pull())
    {
      co_return result;
    }
  }
  std::size_t header_end = buffer.find("\r\n\r\n") + 4;
  result.handshake = buffer.substr(0, header_end);
  buffer.erase(0, header_end);

  if (result.handshake.find("101") == std::string::npos)
  {
    co_return result;
  }

  auto sent = co_await vio::write_tcp(client, reinterpret_cast<const uint8_t *>(send_frames.data()), send_frames.size());
  if (!sent.has_value())
  {
    co_return result;
  }

  server_frame_t echo;
  for (;;)
  {
    echo = parse_server_frame(buffer);
    if (echo.ok)
    {
      break;
    }
    if (!co_await pull())
    {
      co_return result;
    }
  }
  buffer.erase(0, echo.consumed);
  result.echo_payload = echo.payload;
  result.echo_opcode = echo.opcode;

  std::string close = mask_frame(0x8, bytes({0x03, 0xe8}));
  auto closed = co_await vio::write_tcp(client, reinterpret_cast<const uint8_t *>(close.data()), close.size());
  if (!closed.has_value())
  {
    co_return result;
  }

  server_frame_t close_frame;
  for (;;)
  {
    close_frame = parse_server_frame(buffer);
    if (close_frame.ok)
    {
      break;
    }
    if (!co_await pull())
    {
      co_return result;
    }
  }
  result.close_opcode = close_frame.opcode;
  co_return result;
}

vio::task_t<ws_exchange_t> run_ws_case(vio::event_loop_t &loop, std::string path, std::string send_frames)
{
  prism::app_t app;
  app.ws("/echo",
         [](std::shared_ptr<prism::ws_connection_t> connection) -> vio::task_t<void>
         {
           for (;;)
           {
             prism::ws_message_t message = co_await connection->receive();
             if (!message.ok)
             {
               break;
             }
             co_await connection->send_text(message.data);
           }
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

  ws_exchange_t result = co_await run_ws_client(loop, port, std::move(path), std::move(send_frames));

  cancel.cancel();
  auto serve_result = co_await std::move(server_task);
  CHECK(serve_result.has_value());
  co_return result;
}
} // namespace

TEST_CASE("websocket: serialize an unmasked server text frame")
{
  std::string frame = ws::serialize_frame(ws::opcode_t::text, "Hello");
  CHECK(frame == bytes({0x81, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f}));
}

TEST_CASE("websocket: parse a masked client text frame (RFC 6455 vector)")
{
  std::string wire = bytes({0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58});
  ws::frame_reader_t reader;
  CHECK(reader.feed(wire.data(), wire.size()) == ws::frame_status_t::ok);
  REQUIRE(reader.has_frame());
  ws::frame_t frame = reader.take_frame();
  CHECK(frame.fin);
  CHECK(frame.opcode == ws::opcode_t::text);
  CHECK(frame.payload == "Hello");
  CHECK_FALSE(reader.has_frame());
}

TEST_CASE("websocket: an unmasked client frame is a protocol error")
{
  std::string wire = bytes({0x81, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f});
  ws::frame_reader_t reader;
  CHECK(reader.feed(wire.data(), wire.size()) == ws::frame_status_t::error);
  CHECK(reader.error_close_code() == ws::close_code::protocol_error);
}

TEST_CASE("websocket: a byte-at-a-time feed reassembles one frame")
{
  std::string wire = bytes({0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58});
  ws::frame_reader_t reader;
  for (char byte : wire)
  {
    CHECK(reader.feed(&byte, 1) == ws::frame_status_t::ok);
  }
  REQUIRE(reader.has_frame());
  CHECK(reader.take_frame().payload == "Hello");
}

TEST_CASE("websocket: fragmented data frames come through as separate frames")
{
  std::string first = mask_frame(0x1, "he", false);
  std::string second = mask_frame(0x0, "llo", true);
  ws::frame_reader_t reader;
  CHECK(reader.feed(first.data(), first.size()) == ws::frame_status_t::ok);
  CHECK(reader.feed(second.data(), second.size()) == ws::frame_status_t::ok);
  REQUIRE(reader.has_frame());
  ws::frame_t a = reader.take_frame();
  CHECK(a.opcode == ws::opcode_t::text);
  CHECK_FALSE(a.fin);
  CHECK(a.payload == "he");
  REQUIRE(reader.has_frame());
  ws::frame_t b = reader.take_frame();
  CHECK(b.opcode == ws::opcode_t::continuation);
  CHECK(b.fin);
  CHECK(b.payload == "llo");
}

TEST_CASE("websocket: an over-long control frame is a protocol error")
{
  std::string payload(126, 'x');
  std::string wire = mask_frame(0x9, payload);
  ws::frame_reader_t reader;
  CHECK(reader.feed(wire.data(), wire.size()) == ws::frame_status_t::error);
  CHECK(reader.error_close_code() == ws::close_code::protocol_error);
}

TEST_CASE("websocket: serialize a close frame carries the code")
{
  std::string frame = ws::serialize_close(ws::close_code::normal, "");
  CHECK(frame == bytes({0x88, 0x02, 0x03, 0xe8}));
}

TEST_CASE("websocket: accept key matches the RFC 6455 example")
{
  CHECK(ws::accept_key("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST_CASE("websocket: is_upgrade_request validates the handshake headers")
{
  prism::request_t request;
  request.headers.set("Upgrade", "websocket");
  request.headers.set("Connection", "keep-alive, Upgrade");
  request.headers.set("Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==");
  request.headers.set("Sec-WebSocket-Version", "13");
  CHECK(ws::is_upgrade_request(request));

  prism::request_t missing;
  missing.headers.set("Upgrade", "websocket");
  missing.headers.set("Connection", "Upgrade");
  CHECK_FALSE(ws::is_upgrade_request(missing));

  prism::request_t wrong_version;
  wrong_version.headers.set("Upgrade", "websocket");
  wrong_version.headers.set("Connection", "Upgrade");
  wrong_version.headers.set("Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==");
  wrong_version.headers.set("Sec-WebSocket-Version", "8");
  CHECK_FALSE(ws::is_upgrade_request(wrong_version));
}

TEST_CASE("websocket: handshake, echo, and close end to end over TCP")
{
  int rc = vio::run(
    [](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      ws_exchange_t result = co_await run_ws_case(loop, "/echo", mask_frame(0x1, "hello"));
      CHECK(result.handshake.find("HTTP/1.1 101 Switching Protocols") != std::string::npos);
      CHECK(result.handshake.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos);
      CHECK(result.echo_opcode == 0x1);
      CHECK(result.echo_payload == "hello");
      CHECK(result.close_opcode == 0x8);
      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("websocket: the server reassembles fragmented messages before echoing")
{
  int rc = vio::run(
    [](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      std::string frames = mask_frame(0x1, "he", false) + mask_frame(0x0, "llo", true);
      ws_exchange_t result = co_await run_ws_case(loop, "/echo", std::move(frames));
      CHECK(result.echo_opcode == 0x1);
      CHECK(result.echo_payload == "hello");
      co_return 0;
    });
  CHECK(rc == 0);
}
