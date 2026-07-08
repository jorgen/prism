#include <array>
#include <cstddef>
#include <cstdint>
#include <print>
#include <string>
#include <string_view>

#include <prism/app.h>
#include <prism/prism.h>

#include <vio/run.h>
#include <vio/ssl_config_t.h>

namespace
{
// Stream the request body in fixed 64 KiB chunks straight into a reused buffer
// (zero-copy on plain TCP) and report the total byte count -- never buffering
// the whole body in memory.
vio::task_t<prism::response_t> upload(prism::request_t request)
{
  std::array<std::byte, 64u * 1024u> buffer{};
  std::uint64_t total = 0;
  prism::request_body_t &body = request.body_stream();
  for (;;)
  {
    std::size_t n = co_await body.read_into(std::span<std::byte>(buffer.data(), buffer.size()));
    if (n == 0)
    {
      break;
    }
    total += n;
  }
  if (body.status() != prism::body_read_status_t::end)
  {
    co_return prism::response_t::text(prism::status_t::bad_request, "upload interrupted");
  }
  co_return prism::response_t::text(prism::status_t::ok, "received " + std::to_string(total) + " bytes\n");
}

// Same, but drains the whole body into a string via read_all.
vio::task_t<prism::response_t> echo_len(prism::request_t request)
{
  std::string body = co_await request.body_stream().read_all();
  co_return prism::response_t::text(prism::status_t::ok, "received " + std::to_string(body.size()) + " bytes\n");
}
} // namespace

int main(int argc, char **argv)
{
  const std::string_view mode = argc > 1 ? std::string_view(argv[1]) : std::string_view("http1");
  const bool h2c = mode == "h2c";
  const bool tls = mode == "tls";
  const std::string cert = (tls && argc > 2) ? argv[2] : std::string();
  const std::string key = (tls && argc > 3) ? argv[3] : std::string();
  std::println("prism {} — streaming upload ({})", prism::version(), mode);

  return vio::run(
    [h2c, tls, cert, key](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      prism::app_t app;
      app.post_stream("/upload", upload);
      app.post_stream("/echo-len", echo_len);
      app.get("/health",
              [](prism::request_t) -> vio::task_t<prism::response_t>
              {
                co_return prism::response_t::text(prism::status_t::ok, "ok\n");
              });

      prism::server_options_t options;
      if (h2c)
      {
        options.protocol = prism::protocol_t::h2c;
      }
      if (tls)
      {
        vio::ssl_config_t config;
        config.cert_file = cert;
        config.key_file = key;
        config.alpn_protocols = {"http/1.1"};
        auto result = co_await app.listen_tls(loop, "127.0.0.1", 8443, config, nullptr, options);
        if (!result.has_value())
        {
          std::println(stderr, "listen_tls failed: {}", result.error().msg);
          co_return 1;
        }
        co_return 0;
      }
      auto result = co_await app.listen(loop, "127.0.0.1", 8080, nullptr, options);
      if (!result.has_value())
      {
        std::println(stderr, "listen failed: {}", result.error().msg);
        co_return 1;
      }
      co_return 0;
    });
}
