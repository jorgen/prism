#include <doctest/doctest.h>

#include <string>
#include <string_view>

#include <prism/detail/http1.h>

namespace
{
using prism::detail::feed_result_t;
using prism::detail::request_codec_t;
using prism::detail::serialize_response;

feed_result_t feed(request_codec_t &codec, std::string_view bytes)
{
  return codec.feed(bytes.data(), bytes.size());
}
} // namespace

TEST_CASE("codec parses a simple GET request and splits the query off the path")
{
  request_codec_t codec;
  CHECK(feed(codec, "GET /hello?x=1 HTTP/1.1\r\nHost: localhost\r\nAccept: */*\r\n\r\n") == feed_result_t::ok);
  REQUIRE(codec.has_request());

  auto parsed = codec.take_request();
  CHECK(parsed.request.method == prism::method_t::get);
  CHECK(parsed.request.target == "/hello?x=1");
  CHECK(parsed.request.path == "/hello");
  CHECK(parsed.keep_alive);
  REQUIRE(parsed.request.headers.find("host") != nullptr);
  CHECK(*parsed.request.headers.find("host") == "localhost");
  CHECK(*parsed.request.headers.find("accept") == "*/*");
  CHECK_FALSE(codec.has_request());
}

TEST_CASE("codec captures a POST body via Content-Length")
{
  request_codec_t codec;
  CHECK(feed(codec, "POST /users HTTP/1.1\r\nContent-Length: 11\r\n\r\nhello world") == feed_result_t::ok);
  REQUIRE(codec.has_request());

  auto parsed = codec.take_request();
  CHECK(parsed.request.method == prism::method_t::post);
  CHECK(parsed.request.body == "hello world");
}

TEST_CASE("codec reassembles a request split across feeds")
{
  request_codec_t codec;
  CHECK(feed(codec, "GET /split HTTP/1.1\r\nHo") == feed_result_t::ok);
  CHECK_FALSE(codec.has_request());
  CHECK(feed(codec, "st: localhost\r\n\r\n") == feed_result_t::ok);
  REQUIRE(codec.has_request());

  auto parsed = codec.take_request();
  CHECK(parsed.request.path == "/split");
  REQUIRE(parsed.request.headers.find("Host") != nullptr);
  CHECK(*parsed.request.headers.find("Host") == "localhost");
}

TEST_CASE("codec queues pipelined requests from one buffer")
{
  request_codec_t codec;
  CHECK(feed(codec, "GET /a HTTP/1.1\r\n\r\nGET /b HTTP/1.1\r\n\r\n") == feed_result_t::ok);
  REQUIRE(codec.has_request());
  CHECK(codec.take_request().request.path == "/a");
  REQUIRE(codec.has_request());
  CHECK(codec.take_request().request.path == "/b");
  CHECK_FALSE(codec.has_request());
}

TEST_CASE("codec reports keep-alive false on Connection: close")
{
  request_codec_t codec;
  CHECK(feed(codec, "GET /x HTTP/1.1\r\nConnection: close\r\n\r\n") == feed_result_t::ok);
  REQUIRE(codec.has_request());
  CHECK_FALSE(codec.take_request().keep_alive);
}

TEST_CASE("codec decodes a chunked body")
{
  request_codec_t codec;
  CHECK(feed(codec, "POST /c HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n") == feed_result_t::ok);
  REQUIRE(codec.has_request());
  CHECK(codec.take_request().request.body == "hello world");
}

TEST_CASE("codec rejects a malformed request")
{
  request_codec_t codec;
  CHECK(feed(codec, "this is not http\r\n\r\n") == feed_result_t::error);
}

TEST_CASE("serialize_response writes a status line, framing headers, and body")
{
  prism::response_t response = prism::response_t::text(prism::status_t::ok, "pong");
  std::string wire = serialize_response(response, true, false);
  CHECK(wire.starts_with("HTTP/1.1 200 OK\r\n"));
  CHECK(wire.find("Content-Type: text/plain") != std::string::npos);
  CHECK(wire.find("Content-Length: 4\r\n") != std::string::npos);
  CHECK(wire.find("Connection: keep-alive\r\n") != std::string::npos);
  CHECK(wire.find("Date: ") != std::string::npos);
  CHECK(wire.ends_with("\r\n\r\npong"));
}

TEST_CASE("serialize_response honours close and HEAD")
{
  prism::response_t response = prism::response_t::text(prism::status_t::ok, "body");

  std::string closing = serialize_response(response, false, false);
  CHECK(closing.find("Connection: close\r\n") != std::string::npos);

  std::string head = serialize_response(response, true, true);
  CHECK(head.find("Content-Length: 4\r\n") != std::string::npos);
  CHECK(head.ends_with("\r\n\r\n"));
  CHECK(head.find("body") == std::string::npos);
}

TEST_CASE("serialize_response takes authority over client-supplied framing headers")
{
  prism::response_t response;
  response.status = prism::status_t::ok;
  response.headers.set("Content-Length", "999");
  response.headers.set("Connection", "keep-alive");
  response.headers.set("X-Custom", "yes");
  response.body = "abc";

  std::string wire = serialize_response(response, false, false);
  CHECK(wire.find("Content-Length: 999") == std::string::npos);
  CHECK(wire.find("Content-Length: 3\r\n") != std::string::npos);
  CHECK(wire.find("Connection: close\r\n") != std::string::npos);
  CHECK(wire.find("Connection: keep-alive") == std::string::npos);
  CHECK(wire.find("X-Custom: yes\r\n") != std::string::npos);
}

TEST_CASE("serialize_response suppresses a handler-supplied Transfer-Encoding")
{
  prism::response_t response;
  response.status = prism::status_t::ok;
  response.headers.set("Transfer-Encoding", "chunked");
  response.body = "abc";

  std::string wire = serialize_response(response, true, false);
  CHECK(wire.find("Transfer-Encoding") == std::string::npos);
  CHECK(wire.find("Content-Length: 3\r\n") != std::string::npos);
}

TEST_CASE("serialize_response omits Content-Length and body for a 204 response")
{
  prism::response_t response;
  response.status = prism::status_t::no_content;
  response.body = "should not appear";

  std::string wire = serialize_response(response, true, false);
  CHECK(wire.starts_with("HTTP/1.1 204 No Content\r\n"));
  CHECK(wire.find("Content-Length:") == std::string::npos);
  CHECK(wire.find("should not appear") == std::string::npos);
  CHECK(wire.ends_with("\r\n\r\n"));
}
