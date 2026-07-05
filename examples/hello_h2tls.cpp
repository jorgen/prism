#include <cstdlib>
#include <print>
#include <string>

#include <prism/app.h>
#include <prism/json.h>
#include <prism/prism.h>

#include <vio/run.h>
#include <vio/ssl_config_t.h>

namespace
{
struct greeting_t
{
  std::string message;
  std::string to;
  STFY_OBJ(message, to);
};
} // namespace

int main(int argc, char **argv)
{
  if (argc < 3)
  {
    std::println(stderr, "usage: hello_h2tls <cert.pem> <key.pem> [port]");
    return 2;
  }
  std::string cert_file = argv[1];
  std::string key_file = argv[2];
  std::uint16_t port = argc > 3 ? static_cast<std::uint16_t>(std::atoi(argv[3])) : 8443;
  std::println("prism {} — h2/TLS on https://127.0.0.1:{} (ALPN: h2, http/1.1)", prism::version(), port);

  return vio::run(
    [cert_file, key_file, port](vio::event_loop_t &loop) -> vio::task_t<int>
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

      vio::ssl_config_t config;
      config.cert_file = cert_file;
      config.key_file = key_file;
      config.alpn_protocols = {"h2", "http/1.1"};

      auto result = co_await app.listen_tls(loop, "127.0.0.1", port, config);
      if (!result.has_value())
      {
        std::println(stderr, "listen_tls failed: {}", result.error().msg);
        co_return 1;
      }
      co_return 0;
    });
}
