#include <doctest/doctest.h>

#include <charconv>
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
#include <prism/router.h>
#include <prism/server_options.h>

#include <prism/detail/http2/frame.h>
#include <prism/detail/http2/hpack.h>

#include <vio/cancellation.h>
#include <vio/operation/sleep.h>
#include <vio/operation/tcp.h>
#include <vio/operation/tcp_server.h>
#include <vio/run.h>
#include <vio/task.h>

namespace
{
using namespace prism::detail::http2;

struct h2_response_t
{
  int status = 0;
  std::string body;
  bool complete = false;
};

int parse_status(std::string_view value)
{
  int result = 0;
  std::from_chars(value.data(), value.data() + value.size(), result);
  return result;
}

std::string build_request(hpack_encoder_t &encoder, std::uint32_t stream_id, std::string_view method, std::string_view path, std::string_view body)
{
  std::vector<hpack_header_t> headers{
    {":method", std::string(method)},
    {":path", std::string(path)},
    {":scheme", "http"},
    {":authority", "localhost"},
  };
  std::string block = encoder.encode(headers);
  std::string out = serialize_headers(stream_id, block, body.empty(), true);
  if (!body.empty())
  {
    out += serialize_data(stream_id, body, true);
  }
  return out;
}

vio::task_t<std::map<std::uint32_t, h2_response_t>> h2_exchange(vio::event_loop_t &loop, int port, std::string request_bytes, std::vector<std::uint32_t> wait_streams)
{
  std::map<std::uint32_t, h2_response_t> responses;
  auto client_or_error = vio::tcp_create(loop);
  if (!client_or_error.has_value())
  {
    co_return responses;
  }
  auto client = std::move(client_or_error.value());
  auto addr = vio::ip4_addr("127.0.0.1", port);
  if (!addr.has_value())
  {
    co_return responses;
  }
  auto connect = co_await vio::tcp_connect(client, reinterpret_cast<const sockaddr *>(&addr.value()));
  if (!connect.has_value())
  {
    co_return responses;
  }

  constexpr std::uint32_t big_window = 16u * 1024 * 1024;
  std::string preamble(connection_preface);
  preamble += serialize_settings({{static_cast<std::uint16_t>(settings_id_t::initial_window_size), big_window}});
  preamble += serialize_window_update(0, big_window);
  preamble += request_bytes;
  auto write_result = co_await vio::write_tcp(client, reinterpret_cast<const uint8_t *>(preamble.data()), preamble.size());
  if (!write_result.has_value())
  {
    co_return responses;
  }

  auto reader_or_error = vio::tcp_create_reader(client);
  if (!reader_or_error.has_value())
  {
    co_return responses;
  }
  auto reader = std::move(reader_or_error.value());

  vio::cancellation_t token;
  auto watchdog = [](vio::event_loop_t &el, vio::tcp_reader_t &rd, vio::cancellation_t &tok) -> vio::task_t<void>
  {
    auto fired = co_await vio::sleep(el, std::chrono::seconds{5}, &tok);
    if (fired.has_value() && !tok.is_cancelled())
    {
      rd.cancel();
    }
  }(loop, reader, token);

  frame_reader_t frames;
  hpack_decoder_t decoder;
  auto all_done = [&]
  {
    for (std::uint32_t id : wait_streams)
    {
      auto it = responses.find(id);
      if (it == responses.end() || !it->second.complete)
      {
        return false;
      }
    }
    return true;
  };

  while (!all_done())
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
      if (frame.header.type == frame_type_t::headers)
      {
        bool ok = false;
        std::string_view block = headers_block_fragment(frame.header, frame.payload, ok);
        std::vector<hpack_header_t> decoded;
        if (ok && decoder.decode(block, decoded))
        {
          for (const hpack_header_t &header : decoded)
          {
            if (header.name == ":status")
            {
              responses[frame.header.stream_id].status = parse_status(header.value);
            }
          }
        }
        if (frame.header.has_flag(frame_flag::end_stream))
        {
          responses[frame.header.stream_id].complete = true;
        }
      }
      else if (frame.header.type == frame_type_t::data)
      {
        bool ok = false;
        responses[frame.header.stream_id].body += std::string(data_without_padding(frame.header, frame.payload, ok));
        if (frame.header.has_flag(frame_flag::end_stream))
        {
          responses[frame.header.stream_id].complete = true;
        }
      }
    }
  }

  token.cancel();
  co_await std::move(watchdog);
  co_return responses;
}

vio::task_t<std::map<std::uint32_t, h2_response_t>> run_h2_case(vio::event_loop_t &loop, std::string request_bytes, std::vector<std::uint32_t> wait_streams)
{
  auto store = std::make_shared<int>(0);
  prism::app_t app;
  app.get("/ping",
          [](prism::request_t) -> vio::task_t<prism::response_t>
          {
            co_return prism::response_t::text(prism::status_t::ok, "pong");
          });
  app.post("/echo",
           [](prism::request_t request) -> vio::task_t<prism::response_t>
           {
             co_return prism::response_t::text(prism::status_t::ok, request.body);
           });
  app.get("/slow",
          [](prism::request_t request) -> vio::task_t<prism::response_t>
          {
            co_await vio::sleep(*request.loop, std::chrono::milliseconds{40});
            co_return prism::response_t::text(prism::status_t::ok, "slept");
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
  app.get("/bigstream",
          [](prism::request_t) -> vio::task_t<prism::response_t>
          {
            auto counter = std::make_shared<int>(0);
            co_return prism::response_t::streaming(prism::status_t::ok, "application/octet-stream",
                                                   [counter]() -> vio::task_t<prism::body_chunk_t>
                                                   {
                                                     int n = (*counter)++;
                                                     if (n >= 100)
                                                     {
                                                       co_return prism::body_chunk_t{"", true};
                                                     }
                                                     co_return prism::body_chunk_t{std::string(1000, 'x'), false};
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

  prism::keepalive_options_t options;
  options.protocol = prism::protocol_t::h2c;

  vio::cancellation_t cancel;
  auto server_task = prism::detail::serve(std::move(server.value()), std::make_shared<const prism::router_t>(app.router()), nullptr, &cancel, options);

  auto responses = co_await h2_exchange(loop, port, std::move(request_bytes), std::move(wait_streams));

  cancel.cancel();
  auto serve_result = co_await std::move(server_task);
  CHECK(serve_result.has_value());
  co_return responses;
}
} // namespace

TEST_CASE("h2c serves a GET request end to end")
{
  int rc = vio::run(
    [](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      hpack_encoder_t encoder;
      std::string request = build_request(encoder, 1, "GET", "/ping", "");
      auto responses = co_await run_h2_case(loop, std::move(request), {1});
      REQUIRE(responses.count(1) == 1);
      CHECK(responses[1].status == 200);
      CHECK(responses[1].body == "pong");
      CHECK(responses[1].complete);
      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("h2c multiplexes two concurrent streams on one connection")
{
  int rc = vio::run(
    [](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      hpack_encoder_t encoder;
      std::string request = build_request(encoder, 1, "GET", "/ping", "");
      request += build_request(encoder, 3, "GET", "/slow", "");
      auto responses = co_await run_h2_case(loop, std::move(request), {1, 3});
      CHECK(responses[1].status == 200);
      CHECK(responses[1].body == "pong");
      CHECK(responses[3].status == 200);
      CHECK(responses[3].body == "slept");
      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("h2c reads a POST body and echoes it")
{
  int rc = vio::run(
    [](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      hpack_encoder_t encoder;
      std::string request = build_request(encoder, 1, "POST", "/echo", "hello http2");
      auto responses = co_await run_h2_case(loop, std::move(request), {1});
      CHECK(responses[1].status == 200);
      CHECK(responses[1].body == "hello http2");
      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("h2c returns 404 for an unknown route")
{
  int rc = vio::run(
    [](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      hpack_encoder_t encoder;
      std::string request = build_request(encoder, 1, "GET", "/missing", "");
      auto responses = co_await run_h2_case(loop, std::move(request), {1});
      CHECK(responses[1].status == 404);
      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("h2c streams a response as interleaved DATA frames")
{
  int rc = vio::run(
    [](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      hpack_encoder_t encoder;
      std::string request = build_request(encoder, 1, "GET", "/stream", "");
      auto responses = co_await run_h2_case(loop, std::move(request), {1});
      CHECK(responses[1].status == 200);
      CHECK(responses[1].body == "chunk0;chunk1;chunk2;");
      CHECK(responses[1].complete);
      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("h2c streams a large body across the flow-control window")
{
  int rc = vio::run(
    [](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      hpack_encoder_t encoder;
      std::string request = build_request(encoder, 1, "GET", "/bigstream", "");
      auto responses = co_await run_h2_case(loop, std::move(request), {1});
      CHECK(responses[1].status == 200);
      CHECK(responses[1].body.size() == 100000);
      CHECK(responses[1].complete);
      co_return 0;
    });
  CHECK(rc == 0);
}
