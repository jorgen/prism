#include <doctest/doctest.h>

#include <functional>
#include <string>

#include <prism/app.h>

#include <vio/run.h>

namespace
{
struct counter_t
{
  int n = 0;
};

struct label_t
{
  std::string text = "label";
};

prism::request_t make_request(prism::method_t method, std::string target)
{
  prism::request_t request;
  request.method = method;
  request.target = target;
  request.path = target;
  return request;
}

vio::task_t<prism::response_t> bump_a(prism::per_thread<counter_t> c)
{
  c->n += 1;
  co_return prism::response_t::text(prism::status_t::ok, "a=" + std::to_string(c->n));
}

vio::task_t<prism::response_t> bump_b(prism::per_thread<counter_t> c)
{
  c.get().n += 1;
  co_return prism::response_t::text(prism::status_t::ok, "b=" + std::to_string(c->n));
}

vio::task_t<prism::response_t> read_label(prism::per_thread<label_t> l)
{
  co_return prism::response_t::text(prism::status_t::ok, l->text);
}

vio::task_t<prism::response_t> needs_missing(prism::per_thread<label_t> l)
{
  co_return prism::response_t::text(prism::status_t::ok, l->text);
}

vio::task_t<prism::response_t> with_shared(const std::string &prefix, prism::per_thread<counter_t> c)
{
  c->n += 1;
  co_return prism::response_t::text(prism::status_t::ok, prefix + std::to_string(c->n));
}
} // namespace

TEST_CASE("per_thread instance is shared across handlers and mutable across requests")
{
  int rc = vio::run(
    [](vio::event_loop_t &) -> vio::task_t<int>
    {
      prism::app_t app;
      app.provide_per_thread<counter_t>([] { return counter_t{}; });
      app.get("/a", bump_a);
      app.get("/b", bump_b);

      auto r1 = co_await app.handle(make_request(prism::method_t::get, "/a"));
      CHECK(r1.body == "a=1");
      auto r2 = co_await app.handle(make_request(prism::method_t::get, "/b"));
      CHECK(r2.body == "b=2"); // same instance as /a -> continues counting
      auto r3 = co_await app.handle(make_request(prism::method_t::get, "/a"));
      CHECK(r3.body == "a=3");
      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("per_thread with no registered factory yields 500")
{
  int rc = vio::run(
    [](vio::event_loop_t &) -> vio::task_t<int>
    {
      prism::app_t app;
      // note: no provide_per_thread<label_t>
      app.get("/x", needs_missing);
      auto r = co_await app.handle(make_request(prism::method_t::get, "/x"));
      CHECK(r.status == prism::status_t::internal_server_error);
      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("distinct per_thread types get distinct instances")
{
  int rc = vio::run(
    [](vio::event_loop_t &) -> vio::task_t<int>
    {
      prism::app_t app;
      app.provide_per_thread<counter_t>([] { return counter_t{5}; });
      app.provide_per_thread<label_t>([] { return label_t{"hello"}; });
      app.get("/a", bump_a);
      app.get("/label", read_label);

      auto a = co_await app.handle(make_request(prism::method_t::get, "/a"));
      CHECK(a.body == "a=6"); // counter started at 5
      auto l = co_await app.handle(make_request(prism::method_t::get, "/label"));
      CHECK(l.body == "hello"); // independent label instance
      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("shared const bound state coexists with a per_thread extractor")
{
  int rc = vio::run(
    [](vio::event_loop_t &) -> vio::task_t<int>
    {
      prism::app_t app;
      app.provide_per_thread<counter_t>([] { return counter_t{}; });
      std::string prefix = "n=";
      app.get("/both", with_shared, std::cref(prefix));

      auto r = co_await app.handle(make_request(prism::method_t::get, "/both"));
      CHECK(r.body == "n=1");
      co_return 0;
    });
  CHECK(rc == 0);
}
