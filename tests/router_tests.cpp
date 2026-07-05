#include <doctest/doctest.h>

#include <prism/app.h>
#include <prism/json.h>

#include <vio/run.h>

namespace
{
struct user_t
{
  std::string id;
  std::string name;
  STFY_OBJ(id, name);
};

prism::app_t make_app()
{
  prism::app_t app;
  app.get("/users/{id}",
          [](prism::request_t request) -> vio::task_t<prism::response_t>
          {
            user_t user{std::string(request.param("id")), "Ada"};
            co_return prism::json::respond(prism::status_t::ok, user);
          });
  app.post("/users",
           [](prism::request_t request) -> vio::task_t<prism::response_t>
           {
             auto body = prism::json::parse<user_t>(request.body);
             if (!body)
             {
               co_return prism::response_t::text(body.error().code, body.error().msg);
             }
             co_return prism::json::respond(prism::status_t::created, *body);
           });
  return app;
}
} // namespace

TEST_CASE("router binds path params and runs the matching handler")
{
  int rc = vio::run(
    [](vio::event_loop_t &) -> vio::task_t<int>
    {
      prism::app_t app = make_app();

      prism::request_t request;
      request.method = prism::method_t::get;
      request.path = "/users/42";

      prism::response_t response = co_await app.handle(std::move(request));
      CHECK(response.status == prism::status_t::ok);

      auto parsed = prism::json::parse<user_t>(response.body);
      REQUIRE(parsed.has_value());
      CHECK(parsed->id == "42");
      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("router returns 404 for unknown paths and 405 for wrong methods")
{
  int rc = vio::run(
    [](vio::event_loop_t &) -> vio::task_t<int>
    {
      prism::app_t app = make_app();

      prism::request_t missing;
      missing.method = prism::method_t::get;
      missing.path = "/nope";
      prism::response_t r404 = co_await app.handle(std::move(missing));
      CHECK(r404.status == prism::status_t::not_found);

      prism::request_t wrong_method;
      wrong_method.method = prism::method_t::del;
      wrong_method.path = "/users";
      prism::response_t r405 = co_await app.handle(std::move(wrong_method));
      CHECK(r405.status == prism::status_t::method_not_allowed);

      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("router captures a trailing wildcard segment")
{
  int rc = vio::run(
    [](vio::event_loop_t &) -> vio::task_t<int>
    {
      prism::app_t app;
      app.get("/files/{path...}",
              [](prism::request_t request) -> vio::task_t<prism::response_t>
              {
                co_return prism::response_t::text(prism::status_t::ok, std::string(request.param("path")));
              });

      prism::request_t deep;
      deep.method = prism::method_t::get;
      deep.path = "/files/css/app.css";
      prism::response_t res = co_await app.handle(std::move(deep));
      CHECK(res.status == prism::status_t::ok);
      CHECK(res.body == "css/app.css");

      prism::request_t empty_tail;
      empty_tail.method = prism::method_t::get;
      empty_tail.path = "/files";
      prism::response_t res_empty = co_await app.handle(std::move(empty_tail));
      CHECK(res_empty.status == prism::status_t::ok);
      CHECK(res_empty.body.empty());

      co_return 0;
    });
  CHECK(rc == 0);
}
