#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <prism/app.h>
#include <prism/detail/server.h>
#include <prism/detail/http2/frame.h>
#include <prism/detail/http2/hpack.h>
#include <prism/detail/websocket/frame.h>
#include <prism/router.h>
#include <prism/server_options.h>
#include <prism/websocket.h>

#include <vio/cancellation.h>
#include <vio/operation/sleep.h>
#include <vio/operation/tcp.h>
#include <vio/operation/tcp_server.h>
#include <vio/run.h>
#include <vio/task.h>

using namespace prism::detail::http2;
namespace wsf = prism::detail::websocket;

namespace
{
vio::task_t<void> ws_echo(std::shared_ptr<prism::ws_connection_t> connection)
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
}

std::string extended_connect_headers(std::uint32_t stream_id, std::string_view path)
{
  hpack_encoder_t encoder;
  std::vector<hpack_header_t> headers{
    {":method", "CONNECT"},
    {":protocol", "websocket"},
    {":scheme", "https"},
    {":path", std::string(path)},
    {":authority", "localhost"},
  };
  std::string block = encoder.encode(headers);
  return serialize_headers(stream_id, block, false, true);
}

std::string masked_ws(wsf::opcode_t opcode, std::string_view payload)
{
  const std::uint8_t key[4] = {0x11, 0x22, 0x33, 0x44};
  return wsf::serialize_masked_frame(opcode, payload, key, true);
}

struct h2ws_result_t
{
  bool settings_connect_protocol = false;
  int status = 0;
  std::string echo;
  std::uint8_t echo_opcode = 0;
  bool stream_ended = false;
};

vio::task_t<h2ws_result_t> run_h2_ws(vio::event_loop_t &loop)
{
  h2ws_result_t result;

  prism::app_t app;
  app.ws("/echo", ws_echo);

  auto addr = vio::ip4_addr("127.0.0.1", 0);
  REQUIRE(addr.has_value());
  auto server = vio::tcp_create_server(loop);
  REQUIRE(server.has_value());
  REQUIRE(vio::tcp_bind(server.value(), reinterpret_cast<const sockaddr *>(&addr.value())).has_value());
  auto bound_name = vio::sockname(server->tcp);
  REQUIRE(bound_name.has_value());
  int port = ntohs(reinterpret_cast<const sockaddr_in *>(&bound_name.value())->sin_port);

  prism::server_options_t options;
  options.protocol = prism::protocol_t::h2c;
  vio::cancellation_t cancel;
  auto server_task = prism::detail::serve(std::move(server.value()), std::make_shared<const prism::router_t>(app.router()), nullptr, &cancel, options);

  auto client_or_error = vio::tcp_create(loop);
  REQUIRE(client_or_error.has_value());
  auto client = std::move(client_or_error.value());
  auto caddr = vio::ip4_addr("127.0.0.1", port);
  REQUIRE(caddr.has_value());
  auto connected = co_await vio::tcp_connect(client, reinterpret_cast<const sockaddr *>(&caddr.value()));
  REQUIRE(connected.has_value());

  auto send = [&](std::string bytes) -> vio::task_t<bool>
  {
    auto wrote = co_await vio::write_tcp(client, reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size());
    co_return wrote.has_value();
  };

  constexpr std::uint32_t big_window = 16u * 1024 * 1024;
  std::string preamble(connection_preface);
  preamble += serialize_settings({{static_cast<std::uint16_t>(settings_id_t::initial_window_size), big_window}});
  preamble += serialize_window_update(0, big_window);
  preamble += extended_connect_headers(1, "/echo");
  REQUIRE(co_await send(std::move(preamble)));

  auto reader_or_error = vio::tcp_create_reader(client);
  REQUIRE(reader_or_error.has_value());
  auto reader = std::move(reader_or_error.value());

  vio::cancellation_t rtoken;
  auto watchdog = [](vio::event_loop_t &el, vio::tcp_reader_t &rd, vio::cancellation_t &tok) -> vio::task_t<void>
  {
    auto fired = co_await vio::sleep(el, std::chrono::seconds{5}, &tok);
    if (fired.has_value() && !tok.is_cancelled())
    {
      rd.cancel();
    }
  }(loop, reader, rtoken);

  frame_reader_t frames;
  hpack_decoder_t decoder;
  wsf::frame_reader_t ws_in;
  ws_in.set_expect_masked(false);

  int phase = 0;
  bool text_sent = false;
  bool close_sent = false;

  while (!result.stream_ended)
  {
    auto read = co_await reader;
    if (!read.has_value())
    {
      break;
    }
    frames.feed(read.value()->base, read.value()->len);
    while (frames.has_frame())
    {
      frame_t frame = frames.take_frame();
      if (frame.header.type == frame_type_t::settings && !frame.header.has_flag(0x1))
      {
        std::vector<setting_t> parsed;
        if (parse_settings(frame.payload, parsed))
        {
          for (const setting_t &s : parsed)
          {
            if (s.id == static_cast<std::uint16_t>(settings_id_t::enable_connect_protocol) && s.value == 1)
            {
              result.settings_connect_protocol = true;
            }
          }
        }
        REQUIRE(co_await send(serialize_settings_ack()));
      }
      else if (frame.header.type == frame_type_t::headers && frame.header.stream_id == 1)
      {
        bool ok = false;
        std::string_view block = headers_block_fragment(frame.header, frame.payload, ok);
        std::vector<hpack_header_t> decoded;
        if (ok && decoder.decode(block, decoded))
        {
          for (const hpack_header_t &h : decoded)
          {
            if (h.name == ":status")
            {
              std::from_chars(h.value.data(), h.value.data() + h.value.size(), result.status);
            }
          }
        }
      }
      else if (frame.header.type == frame_type_t::data && frame.header.stream_id == 1)
      {
        bool ok = false;
        std::string_view payload = data_without_padding(frame.header, frame.payload, ok);
        if (ok)
        {
          ws_in.feed(payload.data(), payload.size());
        }
        while (ws_in.has_frame())
        {
          wsf::frame_t wf = ws_in.take_frame();
          if (wf.opcode == wsf::opcode_t::text)
          {
            result.echo = wf.payload;
            result.echo_opcode = static_cast<std::uint8_t>(wf.opcode);
          }
        }
        if (frame.header.has_flag(frame_flag::end_stream))
        {
          result.stream_ended = true;
        }
      }
    }

    if (phase == 0 && result.status == 200 && !text_sent)
    {
      text_sent = true;
      phase = 1;
      REQUIRE(co_await send(serialize_data(1, masked_ws(wsf::opcode_t::text, "over-h2"), false)));
    }
    else if (phase == 1 && result.echo == "over-h2" && !close_sent)
    {
      close_sent = true;
      phase = 2;
      std::string close_payload;
      close_payload.push_back(static_cast<char>(0x03));
      close_payload.push_back(static_cast<char>(0xe8));
      REQUIRE(co_await send(serialize_data(1, masked_ws(wsf::opcode_t::close, close_payload), false)));
    }
  }

  rtoken.cancel();
  co_await std::move(watchdog);
  cancel.cancel();
  auto serve_result = co_await std::move(server_task);
  CHECK(serve_result.has_value());
  co_return result;
}

vio::task_t<bool> run_h2_settings_probe(vio::event_loop_t &loop, bool with_ws)
{
  prism::app_t app;
  if (with_ws)
  {
    app.ws("/echo", ws_echo);
  }
  app.get("/plain",
          [](prism::request_t) -> vio::task_t<prism::response_t>
          {
            co_return prism::response_t::text(prism::status_t::ok, "ok");
          });

  auto addr = vio::ip4_addr("127.0.0.1", 0);
  REQUIRE(addr.has_value());
  auto server = vio::tcp_create_server(loop);
  REQUIRE(server.has_value());
  REQUIRE(vio::tcp_bind(server.value(), reinterpret_cast<const sockaddr *>(&addr.value())).has_value());
  auto bound_name = vio::sockname(server->tcp);
  REQUIRE(bound_name.has_value());
  int port = ntohs(reinterpret_cast<const sockaddr_in *>(&bound_name.value())->sin_port);

  prism::server_options_t options;
  options.protocol = prism::protocol_t::h2c;
  vio::cancellation_t cancel;
  auto server_task = prism::detail::serve(std::move(server.value()), std::make_shared<const prism::router_t>(app.router()), nullptr, &cancel, options);

  bool advertised = false;
  auto client_or_error = vio::tcp_create(loop);
  REQUIRE(client_or_error.has_value());
  auto client = std::move(client_or_error.value());
  auto caddr = vio::ip4_addr("127.0.0.1", port);
  REQUIRE(caddr.has_value());
  auto connected = co_await vio::tcp_connect(client, reinterpret_cast<const sockaddr *>(&caddr.value()));
  REQUIRE(connected.has_value());

  std::string preamble(connection_preface);
  preamble += serialize_settings({});
  auto wrote = co_await vio::write_tcp(client, reinterpret_cast<const uint8_t *>(preamble.data()), preamble.size());
  REQUIRE(wrote.has_value());

  auto reader_or_error = vio::tcp_create_reader(client);
  REQUIRE(reader_or_error.has_value());
  auto reader = std::move(reader_or_error.value());

  vio::cancellation_t rtoken;
  auto watchdog = [](vio::event_loop_t &el, vio::tcp_reader_t &rd, vio::cancellation_t &tok) -> vio::task_t<void>
  {
    auto fired = co_await vio::sleep(el, std::chrono::seconds{5}, &tok);
    if (fired.has_value() && !tok.is_cancelled())
    {
      rd.cancel();
    }
  }(loop, reader, rtoken);

  frame_reader_t frames;
  bool got_settings = false;
  while (!got_settings)
  {
    auto read = co_await reader;
    if (!read.has_value())
    {
      break;
    }
    frames.feed(read.value()->base, read.value()->len);
    while (frames.has_frame())
    {
      frame_t frame = frames.take_frame();
      if (frame.header.type == frame_type_t::settings && !frame.header.has_flag(0x1))
      {
        got_settings = true;
        std::vector<setting_t> parsed;
        if (parse_settings(frame.payload, parsed))
        {
          for (const setting_t &s : parsed)
          {
            if (s.id == static_cast<std::uint16_t>(settings_id_t::enable_connect_protocol) && s.value == 1)
            {
              advertised = true;
            }
          }
        }
      }
    }
  }

  rtoken.cancel();
  co_await std::move(watchdog);
  cancel.cancel();
  auto serve_result = co_await std::move(server_task);
  CHECK(serve_result.has_value());
  co_return advertised;
}

struct h2_reset_t
{
  bool saw_rst = false;
  error_code_t code = error_code_t::no_error;
  int status = 0;
};

vio::task_t<h2_reset_t> run_h2_connect_unmatched(vio::event_loop_t &loop, std::string path)
{
  h2_reset_t result;

  prism::app_t app;
  app.ws("/echo", ws_echo);

  auto addr = vio::ip4_addr("127.0.0.1", 0);
  REQUIRE(addr.has_value());
  auto server = vio::tcp_create_server(loop);
  REQUIRE(server.has_value());
  REQUIRE(vio::tcp_bind(server.value(), reinterpret_cast<const sockaddr *>(&addr.value())).has_value());
  auto bound_name = vio::sockname(server->tcp);
  REQUIRE(bound_name.has_value());
  int port = ntohs(reinterpret_cast<const sockaddr_in *>(&bound_name.value())->sin_port);

  prism::server_options_t options;
  options.protocol = prism::protocol_t::h2c;
  vio::cancellation_t cancel;
  auto server_task = prism::detail::serve(std::move(server.value()), std::make_shared<const prism::router_t>(app.router()), nullptr, &cancel, options);

  auto client_or_error = vio::tcp_create(loop);
  REQUIRE(client_or_error.has_value());
  auto client = std::move(client_or_error.value());
  auto caddr = vio::ip4_addr("127.0.0.1", port);
  REQUIRE(caddr.has_value());
  auto connected = co_await vio::tcp_connect(client, reinterpret_cast<const sockaddr *>(&caddr.value()));
  REQUIRE(connected.has_value());

  std::string preamble(connection_preface);
  preamble += serialize_settings({});
  preamble += extended_connect_headers(1, path);
  auto wrote = co_await vio::write_tcp(client, reinterpret_cast<const uint8_t *>(preamble.data()), preamble.size());
  REQUIRE(wrote.has_value());

  auto reader_or_error = vio::tcp_create_reader(client);
  REQUIRE(reader_or_error.has_value());
  auto reader = std::move(reader_or_error.value());

  vio::cancellation_t rtoken;
  auto watchdog = [](vio::event_loop_t &el, vio::tcp_reader_t &rd, vio::cancellation_t &tok) -> vio::task_t<void>
  {
    auto fired = co_await vio::sleep(el, std::chrono::seconds{5}, &tok);
    if (fired.has_value() && !tok.is_cancelled())
    {
      rd.cancel();
    }
  }(loop, reader, rtoken);

  frame_reader_t frames;
  hpack_decoder_t decoder;
  while (!result.saw_rst)
  {
    auto read = co_await reader;
    if (!read.has_value())
    {
      break;
    }
    frames.feed(read.value()->base, read.value()->len);
    while (frames.has_frame())
    {
      frame_t frame = frames.take_frame();
      if (frame.header.type == frame_type_t::settings && !frame.header.has_flag(0x1))
      {
        auto ack = serialize_settings_ack();
        auto sent = co_await vio::write_tcp(client, reinterpret_cast<const uint8_t *>(ack.data()), ack.size());
        REQUIRE(sent.has_value());
      }
      else if (frame.header.type == frame_type_t::headers && frame.header.stream_id == 1)
      {
        bool ok = false;
        std::string_view block = headers_block_fragment(frame.header, frame.payload, ok);
        std::vector<hpack_header_t> decoded;
        if (ok && decoder.decode(block, decoded))
        {
          for (const hpack_header_t &h : decoded)
          {
            if (h.name == ":status")
            {
              std::from_chars(h.value.data(), h.value.data() + h.value.size(), result.status);
            }
          }
        }
      }
      else if (frame.header.type == frame_type_t::rst_stream && frame.header.stream_id == 1)
      {
        error_code_t code = error_code_t::no_error;
        if (parse_rst_stream(frame.payload, code))
        {
          result.saw_rst = true;
          result.code = code;
        }
      }
    }
  }

  rtoken.cancel();
  co_await std::move(watchdog);
  cancel.cancel();
  auto serve_result = co_await std::move(server_task);
  CHECK(serve_result.has_value());
  co_return result;
}
} // namespace

TEST_CASE("h2 8441: server advertises enable_connect_protocol and tunnels a WebSocket echo + close")
{
  int rc = vio::run(
    [](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      h2ws_result_t result = co_await run_h2_ws(loop);
      CHECK(result.settings_connect_protocol);
      CHECK(result.status == 200);
      CHECK(result.echo_opcode == static_cast<std::uint8_t>(wsf::opcode_t::text));
      CHECK(result.echo == "over-h2");
      CHECK(result.stream_ended);
      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("h2 8441: enable_connect_protocol is advertised only when a websocket route exists")
{
  int rc = vio::run(
    [](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      bool with_ws = co_await run_h2_settings_probe(loop, true);
      bool without_ws = co_await run_h2_settings_probe(loop, false);
      CHECK(with_ws);
      CHECK_FALSE(without_ws);
      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("h2 8441: an Extended CONNECT to a non-websocket path is refused, not tunneled")
{
  int rc = vio::run(
    [](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      h2_reset_t result = co_await run_h2_connect_unmatched(loop, "/nope");
      CHECK(result.saw_rst);
      CHECK(result.code == error_code_t::refused_stream);
      CHECK(result.status == 0);
      co_return 0;
    });
  CHECK(rc == 0);
}
