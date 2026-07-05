#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <prism/detail/http2/hpack.h>

namespace
{
using namespace prism::detail::http2;

int hex_nibble(char c)
{
  if (c >= '0' && c <= '9')
  {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f')
  {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F')
  {
    return c - 'A' + 10;
  }
  return 0;
}

std::string unhex(std::string_view hex)
{
  std::string out;
  int high = -1;
  for (char c : hex)
  {
    if (c == ' ')
    {
      continue;
    }
    if (high < 0)
    {
      high = hex_nibble(c);
    }
    else
    {
      out.push_back(static_cast<char>((high << 4) | hex_nibble(c)));
      high = -1;
    }
  }
  return out;
}

bool has_header(const std::vector<hpack_header_t> &headers, std::string_view name, std::string_view value)
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
} // namespace

TEST_CASE("hpack decodes an indexed static field")
{
  hpack_decoder_t decoder;
  std::vector<hpack_header_t> out;
  REQUIRE(decoder.decode(unhex("82"), out));
  REQUIRE(out.size() == 1);
  CHECK(out[0].name == ":method");
  CHECK(out[0].value == "GET");
}

TEST_CASE("hpack decodes RFC 7541 C.3.1 first request without huffman")
{
  hpack_decoder_t decoder;
  std::vector<hpack_header_t> out;
  REQUIRE(decoder.decode(unhex("828684410f7777772e6578616d706c652e636f6d"), out));
  REQUIRE(out.size() == 4);
  CHECK(out[0].name == ":method");
  CHECK(out[0].value == "GET");
  CHECK(out[1].name == ":scheme");
  CHECK(out[1].value == "http");
  CHECK(out[2].name == ":path");
  CHECK(out[2].value == "/");
  CHECK(out[3].name == ":authority");
  CHECK(out[3].value == "www.example.com");
  CHECK(decoder.dynamic_count() == 1);
}

TEST_CASE("hpack resolves a dynamic table reference across header blocks")
{
  hpack_decoder_t decoder;
  std::vector<hpack_header_t> first;
  REQUIRE(decoder.decode(unhex("828684410f7777772e6578616d706c652e636f6d"), first));

  std::vector<hpack_header_t> second;
  REQUIRE(decoder.decode(unhex("828684be58086e6f2d6361636865"), second));
  REQUIRE(second.size() == 5);
  CHECK(has_header(second, ":authority", "www.example.com"));
  CHECK(has_header(second, "cache-control", "no-cache"));
}

TEST_CASE("hpack decodes RFC 7541 C.4.1 first request with huffman")
{
  hpack_decoder_t decoder;
  std::vector<hpack_header_t> out;
  REQUIRE(decoder.decode(unhex("828684418cf1e3c2e5f23a6ba0ab90f4ff"), out));
  REQUIRE(out.size() == 4);
  CHECK(has_header(out, ":authority", "www.example.com"));
}

TEST_CASE("huffman decodes the canonical www.example.com vector")
{
  std::string out;
  REQUIRE(huffman_decode(unhex("f1e3c2e5f23a6ba0ab90f4ff"), out));
  CHECK(out == "www.example.com");
}

TEST_CASE("huffman round-trips arbitrary text including padding")
{
  for (std::string_view sample : {"", "a", "hello world", "no-cache", "Mon, 21 Oct 2013 20:13:22 GMT", "application/json; charset=utf-8"})
  {
    std::string encoded = huffman_encode(sample);
    std::string decoded;
    REQUIRE(huffman_decode(encoded, decoded));
    CHECK(decoded == std::string(sample));
  }
}

TEST_CASE("hpack encoder output decodes back to the original headers")
{
  std::vector<hpack_header_t> headers{
    {":status", "200"},
    {":method", "POST"},
    {"content-type", "application/json"},
    {"x-custom-header", "some-value"},
    {"content-length", "1234"},
  };

  for (bool huffman : {false, true})
  {
    hpack_encoder_t encoder(huffman);
    std::string block = encoder.encode(headers);

    hpack_decoder_t decoder;
    std::vector<hpack_header_t> out;
    REQUIRE(decoder.decode(block, out));
    REQUIRE(out.size() == headers.size());
    for (const hpack_header_t &h : headers)
    {
      CHECK(has_header(out, h.name, h.value));
    }
  }
}

TEST_CASE("hpack round-trips a value long enough to need a multi-byte length")
{
  std::vector<hpack_header_t> headers{{"x-long", std::string(400, 'z')}};
  hpack_encoder_t encoder(false);
  hpack_decoder_t decoder;
  std::vector<hpack_header_t> out;
  REQUIRE(decoder.decode(encoder.encode(headers), out));
  REQUIRE(out.size() == 1);
  CHECK(out[0].value == std::string(400, 'z'));
}

TEST_CASE("hpack evicts the oldest dynamic entry when the table overflows")
{
  hpack_decoder_t decoder(60);
  std::vector<hpack_header_t> out;
  REQUIRE(decoder.decode(unhex("40 03 616161 03 626262"), out));
  CHECK(decoder.dynamic_count() == 1);
  REQUIRE(decoder.decode(unhex("40 03 636363 03 646464"), out));
  CHECK(decoder.dynamic_count() == 1);
  CHECK(decoder.dynamic_size() == 38);
}

TEST_CASE("hpack rejects a truncated string literal")
{
  hpack_decoder_t decoder;
  std::vector<hpack_header_t> out;
  CHECK_FALSE(decoder.decode(unhex("410f7777"), out));
}

TEST_CASE("hpack rejects an indexed field with index zero")
{
  hpack_decoder_t decoder;
  std::vector<hpack_header_t> out;
  CHECK_FALSE(decoder.decode(unhex("80"), out));
}

TEST_CASE("hpack rejects an out-of-range dynamic index")
{
  hpack_decoder_t decoder;
  std::vector<hpack_header_t> out;
  CHECK_FALSE(decoder.decode(unhex("be"), out));
}
