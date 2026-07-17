#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include <prism/acme/csr.h>
#include <prism/acme/manager.h>
#include <prism/app.h>
#include <prism/server_options.h>

#include <vio/operation/tls_common.h>
#include <vio/run.h>
#include <vio/ssl_config_t.h>
#include <vio/ssl_context.h>

namespace
{
std::string env_or(const char *name, const std::string &fallback)
{
  const char *value = std::getenv(name);
  return value != nullptr ? std::string(value) : fallback;
}

std::vector<std::string> split_commas(const std::string &value)
{
  std::vector<std::string> out;
  std::string current;
  for (char c : value)
  {
    if (c == ',')
    {
      if (!current.empty())
      {
        out.push_back(current);
      }
      current.clear();
    }
    else
    {
      current.push_back(c);
    }
  }
  if (!current.empty())
  {
    out.push_back(current);
  }
  return out;
}

std::vector<uint8_t> read_file_bytes(const std::string &path)
{
  std::ifstream input(path, std::ios::binary);
  std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  return {content.begin(), content.end()};
}
} // namespace

int main()
{
  const std::string directory_url = env_or("ACME_DIRECTORY_URL", "https://127.0.0.1:14000/dir");
  const std::string ca_file = env_or("ACME_CA_FILE", "/tmp/pebble/minica.pem");
  const std::vector<std::string> domains = split_commas(env_or("ACME_E2E_DOMAINS", "host1.test,host2.test"));
  const std::string storage_dir = env_or("ACME_E2E_STORAGE", "/tmp/pebble-e2e-storage");
  const auto challenge_port = static_cast<std::uint16_t>(std::atoi(env_or("ACME_E2E_HTTP_PORT", "5002").c_str()));

  std::vector<uint8_t> ca_mem = read_file_bytes(ca_file);
  if (ca_mem.empty())
  {
    std::fprintf(stderr, "e2e: could not read CA file %s\n", ca_file.c_str());
    return 2;
  }

  return vio::run(
    [&](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      prism::app_t challenge_app;

      prism::acme::manager_config_t config;
      config.domains = domains;
      config.contact_email = "e2e@example.test";
      config.directory_url = directory_url;
      config.storage_dir = storage_dir;
      config.ca_mem = ca_mem;
      config.log = [](std::string_view message) { std::fprintf(stderr, "[manager] %.*s\n", static_cast<int>(message.size()), message.data()); };

      prism::acme::manager_t manager(loop, config);
      manager.install_challenge_route(challenge_app);

      vio::cancellation_t serve_cancel;
      prism::server_options_t options;
      options.worker_threads = 1;
      auto serve_task = challenge_app.listen(loop, "", challenge_port, &serve_cancel, options);

      vio::ssl_config_t base_config;
      base_config.alpn_protocols = {"h2", "http/1.1"};
      auto sni_store = vio::make_sni_cert_store(base_config);

      auto ensured = co_await manager.ensure_certificates(sni_store, nullptr);
      if (!ensured)
      {
        std::fprintf(stderr, "e2e: ensure_certificates failed: %s\n", ensured.error().msg.c_str());
      }

      int failures = 0;
      for (const auto &domain : domains)
      {
        if (!sni_store->contains(domain))
        {
          std::fprintf(stderr, "e2e: FAIL no certificate installed for %s\n", domain.c_str());
          ++failures;
          continue;
        }
        std::fprintf(stderr, "e2e: OK certificate installed for %s\n", domain.c_str());
      }

      serve_cancel.cancel();
      co_await std::move(serve_task);

      if (failures == 0)
      {
        std::fprintf(stderr, "e2e: PASS (%zu domains)\n", domains.size());
      }
      co_return failures == 0 ? 0 : 1;
    });
}
