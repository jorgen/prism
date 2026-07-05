#include <cstdint>
#include <cstdlib>
#include <print>
#include <string>

#include <prism/prism.h>

#include <vio/run.h>

namespace
{
struct greeting_t
{
  std::string message;
  STFY_OBJ(message);
};

vio::task_t<prism::negotiated_t<greeting_t>> api_hello(prism::path_t<"name", std::string> name)
{
  co_return prism::ok(greeting_t{"hello " + name.value});
}
} // namespace

int main(int argc, char **argv)
{
  std::string webroot = argc > 1 ? argv[1] : "webroot";
  std::uint16_t port = argc > 2 ? static_cast<std::uint16_t>(std::atoi(argv[2])) : 8080;
  std::println("prism webapp on http://127.0.0.1:{} — static root '{}', REST under /api", port, webroot);

  return vio::run(
    [webroot, port](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      prism::app_t app;

      // REST routes are registered first, so they win over the static mount.
      app.get("/api/hello/{name}", api_hello);

      // Everything else is served from the web root; "/" yields index.html.
      app.static_files("/", webroot);

      auto result = co_await app.listen(loop, "127.0.0.1", port);
      if (!result.has_value())
      {
        std::println(stderr, "listen failed: {}", result.error().msg);
        co_return 1;
      }
      co_return 0;
    });
}
