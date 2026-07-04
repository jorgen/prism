#include <doctest/doctest.h>

#include <cstddef>
#include <optional>
#include <string>

#include <prism/app.h>
#include <prism/codec.h>
#include <prism/content.h>
#include <prism/json.h>
#include <prism/render.h>

#include <vio/run.h>

namespace
{
struct point_t
{
  int x = 0;
  int y = 0;
  STFY_OBJ(x, y);
};

vio::task_t<prism::negotiated_t<point_t>> get_point()
{
  co_return prism::ok(point_t{1, 2});
}

vio::task_t<prism::negotiated_t<point_t>> maybe_point(prism::query_t<"ok", std::optional<int>> flag)
{
  if (!flag.value.has_value())
  {
    co_return prism::response_t::text(prism::status_t::not_found, "nope");
  }
  co_return prism::ok(point_t{*flag.value, 0});
}

vio::task_t<prism::response_t> echo(prism::body_t<point_t> in)
{
  co_return prism::response_t::text(prism::status_t::ok, std::to_string(in.value.x) + "," + std::to_string(in.value.y));
}

vio::task_t<prism::response_t> plain(prism::path_t<"id", int> id)
{
  co_return prism::response_t::text(prism::status_t::ok, "ok" + std::to_string(id.value));
}

vio::task_t<prism::response_t> explicit_point(prism::request_t request)
{
  co_return prism::respond(request, prism::status_t::ok, point_t{11, 12});
}

prism::request_t make_request(prism::method_t method, std::string target, std::string body = {})
{
  prism::request_t request;
  request.method = method;
  request.target = target;
  std::size_t mark = target.find('?');
  request.path = mark == std::string::npos ? target : target.substr(0, mark);
  request.body = std::move(body);
  return request;
}
} // namespace

TEST_CASE("negotiate_accept picks formats by media type and q-value")
{
  using prism::format_t;
  CHECK(prism::negotiate_accept("application/json") == format_t::json);
  CHECK(prism::negotiate_accept("application/yaml") == format_t::yaml);
  CHECK(prism::negotiate_accept("application/cbor") == format_t::cbor);
  CHECK(prism::negotiate_accept("text/yaml") == format_t::yaml);
  CHECK(prism::negotiate_accept("*/*") == format_t::json);
  CHECK(prism::negotiate_accept("application/*") == format_t::json);
  CHECK(prism::negotiate_accept("") == format_t::json);
  CHECK(prism::negotiate_accept("application/json;q=0, application/yaml") == format_t::yaml);
  CHECK(prism::negotiate_accept("application/yaml;q=0.5, application/cbor;q=0.9") == format_t::cbor);
  CHECK(prism::negotiate_accept("text/yaml, application/json") == format_t::json);
  CHECK_FALSE(prism::negotiate_accept("text/html").has_value());
}

TEST_CASE("format_for_content_type maps media types and strips parameters")
{
  using prism::format_t;
  CHECK(prism::format_for_content_type("application/json") == format_t::json);
  CHECK(prism::format_for_content_type("application/yaml; charset=utf-8") == format_t::yaml);
  CHECK(prism::format_for_content_type("application/cbor") == format_t::cbor);
  CHECK_FALSE(prism::format_for_content_type("application/octet-stream").has_value());
}

TEST_CASE("serialize_as and parse_as round-trip each format")
{
  point_t in{3, 4};
  for (prism::format_t format : {prism::format_t::json, prism::format_t::yaml, prism::format_t::cbor})
  {
    prism::serialized_t serialized = prism::serialize_as(format, in);
    auto out = prism::parse_as<point_t>(format, serialized.bytes);
    REQUIRE(out.has_value());
    CHECK(out->x == 3);
    CHECK(out->y == 4);
  }
}

TEST_CASE("response is negotiated from the Accept header")
{
  int rc = vio::run(
    [](vio::event_loop_t &) -> vio::task_t<int>
    {
      prism::app_t app;
      app.get("/point", get_point);

      {
        auto response = co_await app.handle(make_request(prism::method_t::get, "/point"));
        CHECK(response.status == prism::status_t::ok);
        const std::string *content_type = response.headers.find("content-type");
        REQUIRE(content_type != nullptr);
        CHECK(*content_type == "application/json");
        auto parsed = prism::parse_as<point_t>(prism::format_t::json, response.body);
        REQUIRE(parsed.has_value());
        CHECK(parsed->x == 1);
      }
      {
        auto request = make_request(prism::method_t::get, "/point");
        request.headers.set("Accept", "application/yaml");
        auto response = co_await app.handle(std::move(request));
        const std::string *content_type = response.headers.find("content-type");
        REQUIRE(content_type != nullptr);
        CHECK(*content_type == "application/yaml");
        auto parsed = prism::parse_as<point_t>(prism::format_t::yaml, response.body);
        REQUIRE(parsed.has_value());
        CHECK(parsed->y == 2);
      }
      {
        auto request = make_request(prism::method_t::get, "/point");
        request.headers.set("Accept", "application/cbor");
        auto response = co_await app.handle(std::move(request));
        const std::string *content_type = response.headers.find("content-type");
        REQUIRE(content_type != nullptr);
        CHECK(*content_type == "application/cbor");
        auto parsed = prism::parse_as<point_t>(prism::format_t::cbor, response.body);
        REQUIRE(parsed.has_value());
        CHECK(parsed->x == 1);
        CHECK(parsed->y == 2);
      }
      {
        auto request = make_request(prism::method_t::get, "/point");
        request.headers.set("Accept", "text/html");
        auto response = co_await app.handle(std::move(request));
        CHECK(response.status == prism::status_t::not_acceptable);
      }
      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("a negotiated handler can return a ready response that bypasses negotiation")
{
  int rc = vio::run(
    [](vio::event_loop_t &) -> vio::task_t<int>
    {
      prism::app_t app;
      app.get("/maybe", maybe_point);

      {
        auto response = co_await app.handle(make_request(prism::method_t::get, "/maybe"));
        CHECK(response.status == prism::status_t::not_found);
        CHECK(response.body == "nope");
      }
      {
        auto request = make_request(prism::method_t::get, "/maybe?ok=5");
        request.headers.set("Accept", "application/yaml");
        auto response = co_await app.handle(std::move(request));
        CHECK(response.status == prism::status_t::ok);
        const std::string *content_type = response.headers.find("content-type");
        REQUIRE(content_type != nullptr);
        CHECK(*content_type == "application/yaml");
        auto parsed = prism::parse_as<point_t>(prism::format_t::yaml, response.body);
        REQUIRE(parsed.has_value());
        CHECK(parsed->x == 5);
      }
      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("body_t parses according to Content-Type")
{
  int rc = vio::run(
    [](vio::event_loop_t &) -> vio::task_t<int>
    {
      prism::app_t app;
      app.post("/echo", echo);

      {
        auto response = co_await app.handle(make_request(prism::method_t::post, "/echo", "{\"x\":5,\"y\":6}"));
        CHECK(response.status == prism::status_t::ok);
        CHECK(response.body == "5,6");
      }
      {
        auto request = make_request(prism::method_t::post, "/echo", prism::serialize_as(prism::format_t::yaml, point_t{7, 8}).bytes);
        request.headers.set("Content-Type", "application/yaml");
        auto response = co_await app.handle(std::move(request));
        CHECK(response.status == prism::status_t::ok);
        CHECK(response.body == "7,8");
      }
      {
        auto request = make_request(prism::method_t::post, "/echo", prism::serialize_as(prism::format_t::cbor, point_t{9, 10}).bytes);
        request.headers.set("Content-Type", "application/cbor");
        auto response = co_await app.handle(std::move(request));
        CHECK(response.status == prism::status_t::ok);
        CHECK(response.body == "9,10");
      }
      {
        auto request = make_request(prism::method_t::post, "/echo", "{}");
        request.headers.set("Content-Type", "application/octet-stream");
        auto response = co_await app.handle(std::move(request));
        CHECK(response.status == prism::status_t::unsupported_media_type);
      }
      {
        auto request = make_request(prism::method_t::post, "/echo", "not json");
        request.headers.set("Content-Type", "application/json");
        auto response = co_await app.handle(std::move(request));
        CHECK(response.status == prism::status_t::bad_request);
      }
      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("response_t-returning typed handlers still work through the adapter")
{
  int rc = vio::run(
    [](vio::event_loop_t &) -> vio::task_t<int>
    {
      prism::app_t app;
      app.get("/plain/{id}", plain);
      app.get("/explicit", explicit_point);

      {
        auto response = co_await app.handle(make_request(prism::method_t::get, "/plain/42"));
        CHECK(response.status == prism::status_t::ok);
        CHECK(response.body == "ok42");
      }
      {
        auto request = make_request(prism::method_t::get, "/explicit");
        request.headers.set("Accept", "application/yaml");
        auto response = co_await app.handle(std::move(request));
        CHECK(response.status == prism::status_t::ok);
        const std::string *content_type = response.headers.find("content-type");
        REQUIRE(content_type != nullptr);
        CHECK(*content_type == "application/yaml");
      }
      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("finished passes bytes through verbatim with the given content type")
{
  std::string bytes;
  bytes.push_back(static_cast<char>(0x89));
  bytes.push_back('P');
  bytes.push_back('\0');
  bytes.push_back('G');
  prism::response_t response = prism::response_t::finished(prism::status_t::ok, "image/png", bytes);
  CHECK(response.status == prism::status_t::ok);
  const std::string *content_type = response.headers.find("content-type");
  REQUIRE(content_type != nullptr);
  CHECK(*content_type == "image/png");
  REQUIRE(response.body.size() == 4);
  CHECK(response.body == bytes);
}

TEST_CASE("raw_body exposes the request body as bytes")
{
  prism::request_t request;
  request.body = std::string("\x01\x00\x02", 3);
  std::span<const std::byte> bytes = request.raw_body();
  REQUIRE(bytes.size() == 3);
  CHECK(std::to_integer<int>(bytes[0]) == 1);
  CHECK(std::to_integer<int>(bytes[1]) == 0);
  CHECK(std::to_integer<int>(bytes[2]) == 2);
}
