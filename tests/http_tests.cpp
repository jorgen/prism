#include <doctest/doctest.h>

#include <string>
#include <utility>

#include <prism/http.h>

namespace
{
prism::request_t with_target(std::string target)
{
  prism::request_t request;
  request.target = std::move(target);
  return request;
}
} // namespace

TEST_CASE("query returns a named query parameter")
{
  auto request = with_target("/search?q=hello&limit=10");
  CHECK(request.query("q") == "hello");
  CHECK(request.query("limit") == "10");
}

TEST_CASE("query decodes percent-encoding and plus-as-space")
{
  auto request = with_target("/search?q=hello+world%21&tag=c%2B%2B");
  CHECK(request.query("q") == "hello world!");
  CHECK(request.query("tag") == "c++");
}

TEST_CASE("query is empty for an absent parameter or a missing query string")
{
  CHECK(with_target("/search?q=x").query("missing").empty());
  CHECK(with_target("/search").query("q").empty());
}

TEST_CASE("has_query distinguishes a present empty value from an absent one")
{
  auto request = with_target("/search?flag&q=");
  CHECK(request.has_query("flag"));
  CHECK(request.query("flag").empty());
  CHECK(request.has_query("q"));
  CHECK(request.query("q").empty());
  CHECK_FALSE(request.has_query("missing"));
}

TEST_CASE("query decodes encoded keys")
{
  auto request = with_target("/x?a%20b=v");
  CHECK(request.query("a b") == "v");
}
