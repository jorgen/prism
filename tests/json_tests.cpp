#include <doctest/doctest.h>

#include <prism/json.h>

namespace
{
struct user_t
{
  std::string id;
  std::string name;
  int age = 0;
  STFY_OBJ(id, name, age);
};
} // namespace

TEST_CASE("json round-trips a struct through structify")
{
  user_t in{"42", "Ada", 36};
  std::string body = prism::json::serialize(in);

  auto parsed = prism::json::parse<user_t>(body);
  REQUIRE(parsed.has_value());
  CHECK(parsed->id == "42");
  CHECK(parsed->name == "Ada");
  CHECK(parsed->age == 36);
}

TEST_CASE("json::parse reports malformed input as a bad_request error")
{
  auto parsed = prism::json::parse<user_t>("{ this is not json ");
  REQUIRE_FALSE(parsed.has_value());
  CHECK(parsed.error().code == prism::status_t::bad_request);
}

TEST_CASE("json::respond sets an application/json content type")
{
  user_t in{"7", "Grace", 45};
  prism::response_t response = prism::json::respond(prism::status_t::created, in);
  CHECK(response.status == prism::status_t::created);
  const std::string *content_type = response.headers.find("content-type");
  REQUIRE(content_type != nullptr);
  CHECK(*content_type == "application/json");
}
