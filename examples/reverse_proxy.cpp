#include <cstdint>
#include <cstdlib>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include <prism/app.h>
#include <prism/prism.h>
#include <prism/reverse_proxy.h>

#include <vio/run.h>

namespace
{
struct route_spec_t
{
  std::string host;
  std::string backend_host;
  std::uint16_t backend_port;
};

std::optional<route_spec_t> parse_route(std::string_view arg)
{
  std::size_t eq = arg.find('=');
  if (eq == std::string_view::npos)
  {
    return std::nullopt;
  }
  std::string_view host = arg.substr(0, eq);
  std::string_view target = arg.substr(eq + 1);
  std::size_t colon = target.rfind(':');
  if (host.empty() || colon == std::string_view::npos || colon == 0)
  {
    return std::nullopt;
  }
  int port = std::atoi(std::string(target.substr(colon + 1)).c_str());
  if (port <= 0 || port > 65535)
  {
    return std::nullopt;
  }
  return route_spec_t{std::string(host), std::string(target.substr(0, colon)), static_cast<std::uint16_t>(port)};
}
} // namespace

int main(int argc, char **argv)
{
  std::uint16_t port = 8080;
  std::vector<route_spec_t> routes;
  for (int i = 1; i < argc; ++i)
  {
    std::string_view arg = argv[i];
    if (arg.find('=') == std::string_view::npos)
    {
      port = static_cast<std::uint16_t>(std::atoi(argv[i]));
      continue;
    }
    if (std::optional<route_spec_t> spec = parse_route(arg))
    {
      routes.push_back(*spec);
    }
    else
    {
      std::println(stderr, "ignoring malformed route '{}' (expected host=backend_host:port)", arg);
    }
  }

  if (routes.empty())
  {
    std::println("usage: reverse_proxy [listen_port] host=backend_host:port [host2=backend_host:port ...]");
    std::println("example: reverse_proxy 8080 app.localhost=127.0.0.1:9001 api.localhost=127.0.0.1:9002");
    return 1;
  }

  return vio::run(
    [port, routes = std::move(routes)](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      prism::app_t app;
      prism::reverse_proxy_t proxy;
      for (const route_spec_t &spec : routes)
      {
        proxy.add_route(spec.host, prism::backend_t{spec.backend_host, spec.backend_port});
        std::println("Host: {} -> {}:{}", spec.host, spec.backend_host, spec.backend_port);
      }
      proxy.install(app);

      std::println("prism {} — reverse proxy on http://0.0.0.0:{} (HTTP + WebSocket passthrough)", prism::version(), port);
      auto result = co_await app.listen(loop, "", port);
      if (!result.has_value())
      {
        std::println(stderr, "listen failed: {}", result.error().msg);
        co_return 1;
      }
      co_return 0;
    });
}
