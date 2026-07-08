#include <cstdint>
#include <functional>
#include <print>
#include <string>

#include <prism/app.h>
#include <prism/prism.h>

#include <vio/run.h>

namespace
{
// Per-thread mutable state (one instance per thread, produced by a factory).
struct request_counter_t
{
  std::uint64_t seen = 0;
};

// Shared, immutable application state (one instance, bound by const reference).
struct config_t
{
  std::string name;
};

// Bound args first (shared const config), extractors after. per_thread<T> hands
// the handler a mutable reference to this thread's request_counter_t; every route
// that takes it shares the same per-thread instance.
vio::task_t<prism::response_t> hello(const config_t &config, prism::per_thread<request_counter_t> counter, prism::path_t<"who", std::string> who)
{
  counter->seen += 1;
  co_return prism::response_t::text(prism::status_t::ok, config.name + " says hello to " + who.value + " (request #" + std::to_string(counter->seen) + " on this thread)\n");
}

vio::task_t<prism::response_t> stats(prism::per_thread<request_counter_t> counter)
{
  co_return prism::response_t::text(prism::status_t::ok, "this thread has served " + std::to_string(counter->seen) + " hello(s)\n");
}
} // namespace

int main()
{
  std::println("prism {} — per-thread state on http://127.0.0.1:8080 (try /hello/world then /stats)", prism::version());

  return vio::run(
    [](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      prism::app_t app;

      // Register the factory once; prism makes one request_counter_t per thread.
      app.provide_per_thread<request_counter_t>([] { return request_counter_t{}; });

      config_t config{"prism"}; // shared across all threads; lives for the serve
      app.get("/hello/{who}", hello, std::cref(config));
      app.get("/stats", stats); // shares the same per-thread counter as /hello

      auto result = co_await app.listen(loop, "127.0.0.1", 8080);
      if (!result.has_value())
      {
        std::println(stderr, "listen failed: {}", result.error().msg);
        co_return 1;
      }
      co_return 0;
    });
}
