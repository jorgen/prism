#include <print>
#include <string>

#include <prism/app.h>
#include <prism/json.h>
#include <prism/prism.h>

#include <vio/run.h>

namespace
{
struct greeting_t
{
  std::string message;
  std::string to;
  STFY_OBJ(message, to);
};
} // namespace

int main()
{
  std::println("prism {} — listening on http://127.0.0.1:8080", prism::version());

  return vio::run(
    [](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      prism::app_t app;
      app.get("/hello/{name}",
              [](prism::request_t request) -> vio::task_t<prism::response_t>
              {
                greeting_t greeting{"hello", std::string(request.param("name"))};
                co_return prism::respond(request, prism::status_t::ok, greeting);
              });
      app.get("/health",
              [](prism::request_t) -> vio::task_t<prism::response_t>
              {
                co_return prism::response_t::text(prism::status_t::ok, "ok");
              });

      auto result = co_await app.listen(loop, "127.0.0.1", 8080);
      if (!result.has_value())
      {
        std::println(stderr, "listen failed: {}", result.error().msg);
        co_return 1;
      }
      co_return 0;
    });
}
