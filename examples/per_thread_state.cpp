#include <cstdint>
#include <cstdlib>
#include <print>
#include <string>

#include <prism/app.h>
#include <prism/prism.h>
#include <prism/server_options.h>

#include <vio/run.h>

namespace
{
struct request_counter_t
{
  std::uint64_t seen = 0;
};

struct config_t
{
  std::string name;
};

vio::task_t<prism::response_t> hello(const config_t &config, prism::per_thread<request_counter_t> counter, prism::path_t<"who", std::string> who)
{
  counter->seen += 1;
  co_return prism::response_t::text(prism::status_t::ok, config.name + " says hello to " + who.value + " (request #" + std::to_string(counter->seen) + " on this thread)\n");
}

vio::task_t<prism::response_t> stats(prism::per_thread<request_counter_t> counter)
{
  co_return prism::response_t::text(prism::status_t::ok, "this thread has served " + std::to_string(counter->seen) + " hello(s)\n");
}

void configure(prism::app_t &app)
{
  app.provide_per_thread<request_counter_t>([] { return request_counter_t{}; });
  app.get("/hello/{who}", hello, config_t{"prism"});
  app.get("/stats", stats);
}
} // namespace

VIO_MAIN(loop, argc, argv)
{
  std::uint32_t workers = argc > 1 ? static_cast<std::uint32_t>(std::atoi(argv[1])) : 1;
  std::println("prism {} — per-thread state on http://127.0.0.1:8080 with {} worker(s) (try /hello/world then /stats)", prism::version(), workers);

  prism::server_options_t options;
  options.worker_threads = workers;

  co_return co_await prism::run(loop, "", 8080, configure, options);
}
