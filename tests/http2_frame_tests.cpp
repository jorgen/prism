#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <prism/detail/http2/frame.h>

namespace
{
using namespace prism::detail::http2;

frame_status_t feed(frame_reader_t &reader, std::string_view bytes)
{
  return reader.feed(bytes.data(), bytes.size());
}
} // namespace

TEST_CASE("frame header round-trips length, type, flags and stream id")
{
  std::string wire = serialize_frame(frame_type_t::data, frame_flag::end_stream, 5, "abc");

  frame_reader_t reader;
  REQUIRE(feed(reader, wire) == frame_status_t::ok);
  REQUIRE(reader.has_frame());

  frame_t frame = reader.take_frame();
  CHECK(frame.header.length == 3);
  CHECK(frame.header.type == frame_type_t::data);
  CHECK(frame.header.stream_id == 5);
  CHECK(frame.header.has_flag(frame_flag::end_stream));
  CHECK(frame.payload == "abc");
  CHECK_FALSE(reader.has_frame());
}

TEST_CASE("reader clears the reserved bit from the stream id")
{
  std::string wire = serialize_frame(frame_type_t::data, 0, 1, "x");
  wire[5] = static_cast<char>(static_cast<unsigned char>(wire[5]) | 0x80);

  frame_reader_t reader;
  REQUIRE(feed(reader, wire) == frame_status_t::ok);
  REQUIRE(reader.has_frame());
  CHECK(reader.take_frame().header.stream_id == 1);
}

TEST_CASE("reader reassembles a frame split across feeds")
{
  std::string wire = serialize_frame(frame_type_t::ping, 0, 0, "01234567");

  frame_reader_t reader;
  REQUIRE(feed(reader, wire.substr(0, 5)) == frame_status_t::ok);
  CHECK_FALSE(reader.has_frame());
  REQUIRE(feed(reader, wire.substr(5)) == frame_status_t::ok);
  REQUIRE(reader.has_frame());
  CHECK(reader.take_frame().payload == "01234567");
}

TEST_CASE("reader yields multiple frames from one buffer")
{
  std::string wire = serialize_frame(frame_type_t::data, 0, 1, "a");
  wire += serialize_frame(frame_type_t::data, 0, 3, "bb");

  frame_reader_t reader;
  REQUIRE(feed(reader, wire) == frame_status_t::ok);
  REQUIRE(reader.has_frame());
  CHECK(reader.take_frame().header.stream_id == 1);
  REQUIRE(reader.has_frame());
  frame_t second = reader.take_frame();
  CHECK(second.header.stream_id == 3);
  CHECK(second.payload == "bb");
  CHECK_FALSE(reader.has_frame());
}

TEST_CASE("reader rejects a frame larger than the max frame size")
{
  frame_reader_t reader;
  reader.set_max_frame_size(default_max_frame_size);
  std::string oversized = serialize_frame(frame_type_t::data, 0, 1, std::string(default_max_frame_size + 1, 'x'));
  CHECK(feed(reader, oversized) == frame_status_t::error);
  CHECK(reader.error() == error_code_t::frame_size_error);
}

TEST_CASE("settings serialize and parse round-trip")
{
  std::vector<setting_t> in{
    {static_cast<std::uint16_t>(settings_id_t::max_concurrent_streams), 100},
    {static_cast<std::uint16_t>(settings_id_t::initial_window_size), 65535},
  };
  std::string wire = serialize_settings(in);

  frame_reader_t reader;
  REQUIRE(feed(reader, wire) == frame_status_t::ok);
  REQUIRE(reader.has_frame());
  frame_t frame = reader.take_frame();
  CHECK(frame.header.type == frame_type_t::settings);
  CHECK_FALSE(frame.header.has_flag(frame_flag::ack));

  std::vector<setting_t> out;
  REQUIRE(parse_settings(frame.payload, out));
  REQUIRE(out.size() == 2);
  CHECK(out[0].id == static_cast<std::uint16_t>(settings_id_t::max_concurrent_streams));
  CHECK(out[0].value == 100);
  CHECK(out[1].value == 65535);
}

TEST_CASE("settings ack carries the ack flag and an empty payload")
{
  frame_reader_t reader;
  REQUIRE(feed(reader, serialize_settings_ack()) == frame_status_t::ok);
  REQUIRE(reader.has_frame());
  frame_t frame = reader.take_frame();
  CHECK(frame.header.type == frame_type_t::settings);
  CHECK(frame.header.has_flag(frame_flag::ack));
  CHECK(frame.payload.empty());
}

TEST_CASE("parse_settings rejects a payload that is not a multiple of six")
{
  std::vector<setting_t> out;
  CHECK_FALSE(parse_settings(std::string(5, '\0'), out));
}

TEST_CASE("ping ack echoes the opaque payload")
{
  frame_reader_t reader;
  REQUIRE(feed(reader, serialize_ping("PINGDATA", true)) == frame_status_t::ok);
  REQUIRE(reader.has_frame());
  frame_t frame = reader.take_frame();
  CHECK(frame.header.type == frame_type_t::ping);
  CHECK(frame.header.has_flag(frame_flag::ack));
  CHECK(frame.payload == "PINGDATA");
}

TEST_CASE("window_update round-trips the increment")
{
  frame_reader_t reader;
  REQUIRE(feed(reader, serialize_window_update(3, 1024)) == frame_status_t::ok);
  REQUIRE(reader.has_frame());
  frame_t frame = reader.take_frame();
  CHECK(frame.header.stream_id == 3);
  std::uint32_t increment = 0;
  REQUIRE(parse_window_update(frame.payload, increment));
  CHECK(increment == 1024);
}

TEST_CASE("rst_stream round-trips the error code")
{
  frame_reader_t reader;
  REQUIRE(feed(reader, serialize_rst_stream(7, error_code_t::cancel)) == frame_status_t::ok);
  REQUIRE(reader.has_frame());
  frame_t frame = reader.take_frame();
  CHECK(frame.header.stream_id == 7);
  error_code_t code = error_code_t::no_error;
  REQUIRE(parse_rst_stream(frame.payload, code));
  CHECK(code == error_code_t::cancel);
}

TEST_CASE("goaway round-trips last stream id, code and debug data")
{
  frame_reader_t reader;
  REQUIRE(feed(reader, serialize_goaway(9, error_code_t::protocol_error, "bad frame")) == frame_status_t::ok);
  REQUIRE(reader.has_frame());
  frame_t frame = reader.take_frame();
  goaway_t out;
  REQUIRE(parse_goaway(frame.payload, out));
  CHECK(out.last_stream_id == 9);
  CHECK(out.code == error_code_t::protocol_error);
  CHECK(out.debug == "bad frame");
}

TEST_CASE("data_without_padding strips the pad length prefix and trailing padding")
{
  std::string payload;
  payload.push_back(static_cast<char>(3));
  payload += "hello";
  payload.append(3, '\0');

  frame_header_t header;
  header.type = frame_type_t::data;
  header.flags = frame_flag::padded;

  bool ok = false;
  std::string_view data = data_without_padding(header, payload, ok);
  REQUIRE(ok);
  CHECK(std::string(data) == "hello");
}

TEST_CASE("data_without_padding rejects a pad length larger than the payload")
{
  std::string payload;
  payload.push_back(static_cast<char>(200));
  payload += "hi";

  frame_header_t header;
  header.type = frame_type_t::data;
  header.flags = frame_flag::padded;

  bool ok = true;
  data_without_padding(header, payload, ok);
  CHECK_FALSE(ok);
}

TEST_CASE("headers_block_fragment skips padding and the priority field")
{
  std::string payload;
  payload.push_back(static_cast<char>(2));
  payload.append(5, '\0');
  payload += "blockbytes";
  payload.append(2, '\0');

  frame_header_t header;
  header.type = frame_type_t::headers;
  header.flags = static_cast<std::uint8_t>(frame_flag::padded | frame_flag::priority);

  bool ok = false;
  std::string_view block = headers_block_fragment(header, payload, ok);
  REQUIRE(ok);
  CHECK(std::string(block) == "blockbytes");
}

TEST_CASE("headers_block_fragment returns the whole payload when unpadded and unprioritised")
{
  frame_header_t header;
  header.type = frame_type_t::headers;
  header.flags = frame_flag::end_headers;

  bool ok = false;
  std::string_view block = headers_block_fragment(header, "rawblock", ok);
  REQUIRE(ok);
  CHECK(std::string(block) == "rawblock");
}
