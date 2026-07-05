#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <prism/detail/http2/connection.h>
#include <prism/detail/http2/frame.h>
#include <prism/detail/http2/hpack.h>
#include <prism/http.h>

namespace
{
using namespace prism::detail::http2;

std::string client_headers(hpack_encoder_t &encoder, std::uint32_t stream_id, const std::vector<hpack_header_t> &headers, bool end_stream)
{
  std::string block = encoder.encode(headers);
  return serialize_headers(stream_id, block, end_stream, true);
}

std::string preface_and_settings()
{
  std::string out(connection_preface);
  out += serialize_settings({});
  return out;
}

std::vector<frame_t> parse_frames(const std::string &bytes)
{
  frame_reader_t reader;
  reader.feed(bytes.data(), bytes.size());
  std::vector<frame_t> frames;
  while (reader.has_frame())
  {
    frames.push_back(reader.take_frame());
  }
  return frames;
}

std::optional<std::vector<hpack_header_t>> response_headers(const std::vector<frame_t> &frames, std::uint32_t stream_id)
{
  hpack_decoder_t decoder;
  for (const frame_t &frame : frames)
  {
    if (frame.header.type == frame_type_t::headers && frame.header.stream_id == stream_id)
    {
      bool ok = false;
      std::string_view block = headers_block_fragment(frame.header, frame.payload, ok);
      if (!ok)
      {
        return std::nullopt;
      }
      std::vector<hpack_header_t> decoded;
      if (!decoder.decode(block, decoded))
      {
        return std::nullopt;
      }
      return decoded;
    }
  }
  return std::nullopt;
}

std::string response_body(const std::vector<frame_t> &frames, std::uint32_t stream_id, bool &end_seen)
{
  std::string body;
  end_seen = false;
  for (const frame_t &frame : frames)
  {
    if (frame.header.type == frame_type_t::data && frame.header.stream_id == stream_id)
    {
      bool ok = false;
      body += std::string(data_without_padding(frame.header, frame.payload, ok));
      if (frame.header.has_flag(frame_flag::end_stream))
      {
        end_seen = true;
      }
    }
  }
  return body;
}

bool header_equals(const std::vector<hpack_header_t> &headers, std::string_view name, std::string_view value)
{
  for (const hpack_header_t &h : headers)
  {
    if (h.name == name && h.value == value)
    {
      return true;
    }
  }
  return false;
}

bool has_goaway(const std::vector<frame_t> &frames, error_code_t code)
{
  for (const frame_t &frame : frames)
  {
    if (frame.header.type == frame_type_t::goaway)
    {
      goaway_t out;
      if (parse_goaway(frame.payload, out) && out.code == code)
      {
        return true;
      }
    }
  }
  return false;
}

bool has_rst(const std::vector<frame_t> &frames, std::uint32_t stream_id, error_code_t code)
{
  for (const frame_t &frame : frames)
  {
    if (frame.header.type == frame_type_t::rst_stream && frame.header.stream_id == stream_id)
    {
      error_code_t got = error_code_t::no_error;
      if (parse_rst_stream(frame.payload, got) && got == code)
      {
        return true;
      }
    }
  }
  return false;
}
} // namespace

TEST_CASE("connection assembles a GET request and encodes a response")
{
  connection_t conn;
  conn.start();
  hpack_encoder_t encoder;

  std::string input = preface_and_settings();
  input += client_headers(encoder, 1,
                          {{":method", "GET"}, {":path", "/hello?x=1"}, {":scheme", "http"}, {":authority", "example.com"}}, true);

  std::vector<ready_request_t> ready;
  REQUIRE(conn.receive(input, ready));
  REQUIRE(ready.size() == 1);
  CHECK(ready[0].stream_id == 1);
  CHECK(ready[0].request.method == prism::method_t::get);
  CHECK(ready[0].request.path == "/hello");
  CHECK(ready[0].request.target == "/hello?x=1");
  REQUIRE(ready[0].request.headers.find("host") != nullptr);
  CHECK(*ready[0].request.headers.find("host") == "example.com");

  conn.submit_response(1, prism::response_t::text(prism::status_t::ok, "pong"), false);
  std::vector<frame_t> frames = parse_frames(conn.take_output());

  auto headers = response_headers(frames, 1);
  REQUIRE(headers.has_value());
  CHECK(header_equals(*headers, ":status", "200"));
  CHECK(header_equals(*headers, "content-type", "text/plain; charset=utf-8"));
  CHECK(header_equals(*headers, "content-length", "4"));

  bool end_seen = false;
  CHECK(response_body(frames, 1, end_seen) == "pong");
  CHECK(end_seen);
}

TEST_CASE("connection captures a POST body split across DATA frames")
{
  connection_t conn;
  conn.start();
  hpack_encoder_t encoder;

  std::string input = preface_and_settings();
  input += client_headers(encoder, 1,
                          {{":method", "POST"}, {":path", "/upload"}, {":scheme", "http"}, {":authority", "h"}}, false);
  input += serialize_data(1, "hello ", false);
  input += serialize_data(1, "world", true);

  std::vector<ready_request_t> ready;
  REQUIRE(conn.receive(input, ready));
  REQUIRE(ready.size() == 1);
  CHECK(ready[0].request.method == prism::method_t::post);
  CHECK(ready[0].request.body == "hello world");
}

TEST_CASE("connection multiplexes two concurrent streams")
{
  connection_t conn;
  conn.start();
  hpack_encoder_t encoder;

  std::string input = preface_and_settings();
  input += client_headers(encoder, 1, {{":method", "GET"}, {":path", "/a"}, {":scheme", "http"}}, true);
  input += client_headers(encoder, 3, {{":method", "GET"}, {":path", "/b"}, {":scheme", "http"}}, true);

  std::vector<ready_request_t> ready;
  REQUIRE(conn.receive(input, ready));
  REQUIRE(ready.size() == 2);
  CHECK(ready[0].stream_id == 1);
  CHECK(ready[0].request.path == "/a");
  CHECK(ready[1].stream_id == 3);
  CHECK(ready[1].request.path == "/b");

  conn.submit_response(3, prism::response_t::text(prism::status_t::ok, "B"), false);
  conn.submit_response(1, prism::response_t::text(prism::status_t::ok, "A"), false);
  std::vector<frame_t> frames = parse_frames(conn.take_output());

  bool end1 = false;
  bool end3 = false;
  CHECK(response_body(frames, 1, end1) == "A");
  CHECK(response_body(frames, 3, end3) == "B");
}

TEST_CASE("connection answers a PING with a PING ACK")
{
  connection_t conn;
  conn.start();

  std::string input = preface_and_settings();
  input += serialize_ping("01234567", false);

  std::vector<ready_request_t> ready;
  REQUIRE(conn.receive(input, ready));
  std::vector<frame_t> frames = parse_frames(conn.take_output());

  bool found = false;
  for (const frame_t &frame : frames)
  {
    if (frame.header.type == frame_type_t::ping && frame.header.has_flag(frame_flag::ack))
    {
      found = true;
      CHECK(frame.payload == "01234567");
    }
  }
  CHECK(found);
}

TEST_CASE("connection rejects a bad preface with GOAWAY")
{
  connection_t conn;
  conn.start();

  std::vector<ready_request_t> ready;
  CHECK_FALSE(conn.receive("GET / HTTP/1.1\r\n\r\nnot a preface at all!!", ready));
  CHECK(conn.failed());

  std::vector<frame_t> frames = parse_frames(conn.take_output());
  bool goaway = false;
  for (const frame_t &frame : frames)
  {
    if (frame.header.type == frame_type_t::goaway)
    {
      goaway = true;
    }
  }
  CHECK(goaway);
}

TEST_CASE("connection honours the peer send window and resumes on WINDOW_UPDATE")
{
  connection_t conn;
  conn.start();
  hpack_encoder_t encoder;

  std::string settings = serialize_settings({{static_cast<std::uint16_t>(settings_id_t::initial_window_size), 5}});
  std::string input(connection_preface);
  input += settings;
  input += client_headers(encoder, 1, {{":method", "GET"}, {":path", "/big"}, {":scheme", "http"}}, true);

  std::vector<ready_request_t> ready;
  REQUIRE(conn.receive(input, ready));
  REQUIRE(ready.size() == 1);

  conn.submit_response(1, prism::response_t::text(prism::status_t::ok, "0123456789"), false);

  std::vector<frame_t> first = parse_frames(conn.take_output());
  bool end_partial = false;
  std::string partial = response_body(first, 1, end_partial);
  CHECK(partial.size() == 5);
  CHECK_FALSE(end_partial);

  std::vector<ready_request_t> ready2;
  REQUIRE(conn.receive(serialize_window_update(1, 5), ready2));
  std::vector<frame_t> second = parse_frames(conn.take_output());
  bool end_final = false;
  std::string rest = response_body(second, 1, end_final);
  CHECK(rest.size() == 5);
  CHECK(end_final);
  CHECK(partial + rest == "0123456789");
}

TEST_CASE("connection reassembles a header block split into CONTINUATION frames")
{
  connection_t conn;
  conn.start();
  hpack_encoder_t encoder;

  std::string block = encoder.encode({{":method", "GET"}, {":path", "/split"}, {":scheme", "http"}});
  std::size_t half = block.size() / 2;
  std::string input = preface_and_settings();
  input += serialize_headers(1, std::string_view(block).substr(0, half), true, false);
  input += serialize_continuation(1, std::string_view(block).substr(half), true);

  std::vector<ready_request_t> ready;
  REQUIRE(conn.receive(input, ready));
  REQUIRE(ready.size() == 1);
  CHECK(ready[0].request.path == "/split");
}

TEST_CASE("connection tears down on a rapid-reset flood")
{
  h2_settings_t settings;
  settings.max_reset_streams = 2;
  connection_t conn(settings);
  conn.start();
  hpack_encoder_t encoder;

  std::string input = preface_and_settings();
  for (std::uint32_t id : {1u, 3u, 5u})
  {
    input += client_headers(encoder, id, {{":method", "GET"}, {":path", "/x"}, {":scheme", "http"}}, false);
    input += serialize_rst_stream(id, error_code_t::cancel);
  }

  std::vector<ready_request_t> ready;
  CHECK_FALSE(conn.receive(input, ready));
  CHECK(has_goaway(parse_frames(conn.take_output()), error_code_t::enhance_your_calm));
}

TEST_CASE("connection tears down on a SETTINGS flood")
{
  h2_settings_t settings;
  settings.max_pending_settings_ack = 2;
  connection_t conn(settings);
  conn.start();

  std::string input(connection_preface);
  input += serialize_settings({});
  input += serialize_settings({});
  input += serialize_settings({});

  std::vector<ready_request_t> ready;
  CHECK_FALSE(conn.receive(input, ready));
  CHECK(has_goaway(parse_frames(conn.take_output()), error_code_t::enhance_your_calm));
}

TEST_CASE("connection refuses streams beyond the concurrency limit")
{
  h2_settings_t settings;
  settings.max_concurrent_streams = 1;
  connection_t conn(settings);
  conn.start();
  hpack_encoder_t encoder;

  std::string input = preface_and_settings();
  input += client_headers(encoder, 1, {{":method", "GET"}, {":path", "/a"}, {":scheme", "http"}}, false);
  input += client_headers(encoder, 3, {{":method", "GET"}, {":path", "/b"}, {":scheme", "http"}}, false);

  std::vector<ready_request_t> ready;
  REQUIRE(conn.receive(input, ready));
  CHECK(has_rst(parse_frames(conn.take_output()), 3, error_code_t::refused_stream));
}

TEST_CASE("connection rejects a header list that exceeds the decompressed cap")
{
  h2_settings_t settings;
  settings.max_header_list_size = 40;
  connection_t conn(settings);
  conn.start();
  hpack_encoder_t encoder;

  std::string input = preface_and_settings();
  input += client_headers(encoder, 1,
                          {{":method", "GET"}, {":path", "/x"}, {":scheme", "http"}, {"x-big", std::string(200, 'v')}}, true);

  std::vector<ready_request_t> ready;
  CHECK_FALSE(conn.receive(input, ready));
  CHECK(has_goaway(parse_frames(conn.take_output()), error_code_t::compression_error));
}

TEST_CASE("connection sends a bodyless response without DATA")
{
  connection_t conn;
  conn.start();
  hpack_encoder_t encoder;

  std::string input = preface_and_settings();
  input += client_headers(encoder, 1, {{":method", "DELETE"}, {":path", "/x"}, {":scheme", "http"}}, true);

  std::vector<ready_request_t> ready;
  REQUIRE(conn.receive(input, ready));

  prism::response_t response;
  response.status = prism::status_t::no_content;
  conn.submit_response(1, response, false);

  std::vector<frame_t> frames = parse_frames(conn.take_output());
  auto headers = response_headers(frames, 1);
  REQUIRE(headers.has_value());
  CHECK(header_equals(*headers, ":status", "204"));

  bool has_data = false;
  for (const frame_t &frame : frames)
  {
    if (frame.header.type == frame_type_t::data && frame.header.stream_id == 1)
    {
      has_data = true;
    }
  }
  CHECK_FALSE(has_data);

  bool headers_end_stream = false;
  for (const frame_t &frame : frames)
  {
    if (frame.header.type == frame_type_t::headers && frame.header.stream_id == 1)
    {
      headers_end_stream = frame.header.has_flag(frame_flag::end_stream);
    }
  }
  CHECK(headers_end_stream);
}
