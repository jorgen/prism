#include <doctest/doctest.h>

#include <memory>
#include <optional>
#include <string>

#include <prism/app.h>
#include <prism/json.h>

#include <vio/run.h>

namespace
{
struct item_t
{
  int id = 0;
  std::string name;
  STFY_OBJ(id, name);
};

vio::task_t<prism::response_t> show(prism::path_t<"id", int> id)
{
  co_return prism::response_t::text(prism::status_t::ok, "id=" + std::to_string(id.value));
}

vio::task_t<prism::response_t> search(prism::query_t<"q", std::string> q, prism::query_t<"limit", std::optional<int>> limit)
{
  std::string out = q.value;
  if (limit.value.has_value())
  {
    out += ":" + std::to_string(*limit.value);
  }
  co_return prism::response_t::text(prism::status_t::ok, out);
}

vio::task_t<prism::response_t> create(prism::body_t<item_t> in)
{
  co_return prism::json::respond(prism::status_t::created, in.value);
}

vio::task_t<prism::response_t> greet(std::shared_ptr<std::string> prefix, prism::path_t<"name", std::string> name)
{
  co_return prism::response_t::text(prism::status_t::ok, *prefix + name.value);
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

TEST_CASE("typed path parameter is parsed and injected")
{
  int rc = vio::run(
    [](vio::event_loop_t &) -> vio::task_t<int>
    {
      prism::app_t app;
      app.get("/things/{id}", show);

      auto ok = co_await app.handle(make_request(prism::method_t::get, "/things/42"));
      CHECK(ok.status == prism::status_t::ok);
      CHECK(ok.body == "id=42");

      auto bad = co_await app.handle(make_request(prism::method_t::get, "/things/abc"));
      CHECK(bad.status == prism::status_t::bad_request);

      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("typed query parameters: required, optional, and missing")
{
  int rc = vio::run(
    [](vio::event_loop_t &) -> vio::task_t<int>
    {
      prism::app_t app;
      app.get("/search", search);

      auto with_limit = co_await app.handle(make_request(prism::method_t::get, "/search?q=hello&limit=5"));
      CHECK(with_limit.body == "hello:5");

      auto no_limit = co_await app.handle(make_request(prism::method_t::get, "/search?q=hello"));
      CHECK(no_limit.body == "hello");

      auto missing_required = co_await app.handle(make_request(prism::method_t::get, "/search?limit=5"));
      CHECK(missing_required.status == prism::status_t::bad_request);

      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("typed body parameter binds JSON and 400s on malformed input")
{
  int rc = vio::run(
    [](vio::event_loop_t &) -> vio::task_t<int>
    {
      prism::app_t app;
      app.post("/items", create);

      auto ok = co_await app.handle(make_request(prism::method_t::post, "/items", "{\"id\":7,\"name\":\"widget\"}"));
      CHECK(ok.status == prism::status_t::created);
      CHECK(ok.body.find("\"id\":7") != std::string::npos);

      auto bad = co_await app.handle(make_request(prism::method_t::post, "/items", "not json"));
      CHECK(bad.status == prism::status_t::bad_request);

      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("typed handler binds leading state arguments")
{
  int rc = vio::run(
    [](vio::event_loop_t &) -> vio::task_t<int>
    {
      prism::app_t app;
      app.get("/hi/{name}", greet, std::make_shared<std::string>("hello "));

      auto r = co_await app.handle(make_request(prism::method_t::get, "/hi/ada"));
      CHECK(r.body == "hello ada");

      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("route registration flags a handler binding an undeclared path parameter")
{
  prism::app_t app;
  app.get("/things/{id}", show);
  app.get("/people/{name}", greet, std::make_shared<std::string>("hi "));
  CHECK(app.route_errors().empty());

  app.get("/widgets/{wid}", show);
  REQUIRE(app.route_errors().size() == 1);
  CHECK(app.route_errors().front().find("'id'") != std::string::npos);
  CHECK(app.route_errors().front().find("/widgets/{wid}") != std::string::npos);
}

TEST_CASE("a statically verified route parses and dispatches")
{
  int rc = vio::run(
    [](vio::event_loop_t &) -> vio::task_t<int>
    {
      prism::app_t app;
      app.get<"/things/{id}">(show);
      app.get<"/hi/{name}">(greet, std::make_shared<std::string>("hello "));

      auto a = co_await app.handle(make_request(prism::method_t::get, "/things/42"));
      CHECK(a.body == "id=42");

      auto b = co_await app.handle(make_request(prism::method_t::get, "/hi/ada"));
      CHECK(b.body == "hello ada");

      CHECK(app.route_errors().empty());

      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("listen fails fast on a route configuration error")
{
  int rc = vio::run(
    [](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      prism::app_t app;
      app.get("/widgets/{wid}", show);

      auto result = co_await app.listen(loop, "127.0.0.1", 0);
      CHECK_FALSE(result.has_value());
      CHECK(result.error().code == prism::status_t::internal_server_error);

      co_return 0;
    });
  CHECK(rc == 0);
}
