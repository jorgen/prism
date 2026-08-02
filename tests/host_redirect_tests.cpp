#include <doctest/doctest.h>

#include <string>
#include <utility>

#include <prism/host_redirect.h>
#include <prism/http.h>
#include <prism/status.h>

#include <vio/run.h>
#include <vio/task.h>

namespace
{
vio::task_t<prism::response_t> inner_ok(prism::request_t)
{
  prism::response_t response;
  response.status = prism::status_t::ok;
  response.body = "inner";
  co_return response;
}

prism::request_t make_request(std::string host, std::string target)
{
  prism::request_t request;
  request.headers.set("Host", std::move(host));
  request.target = std::move(target);
  return request;
}

TEST_SUITE("host_redirect")
{
  TEST_CASE("redirects a matching host with path and query preserved; others pass through")
  {
    const int rc = vio::run(
      [](vio::event_loop_t &) -> vio::task_t<int>
      {
        prism::host_redirect_t redirect;
        redirect.add("old.example.com", "new.example.com");
        auto handler = redirect.wrap(&inner_ok);

        auto redirected = co_await handler(make_request("old.example.com", "/a/b?q=1"));
        CHECK(redirected.status == prism::status_t::permanent_redirect);
        const std::string *location = redirected.headers.find("Location");
        REQUIRE(location != nullptr);
        CHECK(*location == "https://new.example.com/a/b?q=1");

        // Host matching ignores case and any :port suffix.
        auto with_port = co_await handler(make_request("OLD.Example.COM:443", "/"));
        CHECK(with_port.status == prism::status_t::permanent_redirect);

        auto passthrough = co_await handler(make_request("new.example.com", "/a"));
        CHECK(passthrough.status == prism::status_t::ok);
        CHECK(passthrough.body == "inner");

        auto no_host = co_await handler(prism::request_t{});
        CHECK(no_host.status == prism::status_t::ok);
        co_return 0;
      });
    CHECK(rc == 0);
  }

  TEST_CASE("empty() reflects registrations")
  {
    prism::host_redirect_t redirect;
    CHECK(redirect.empty());
    redirect.add("a.example.com", "b.example.com");
    CHECK(!redirect.empty());
  }
}
} // namespace
