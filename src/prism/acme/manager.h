#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <vio/cancellation.h>
#include <vio/event_loop.h>
#include <vio/task.h>

#include "../error.h"
#include "client.h"

namespace vio
{
struct sni_cert_store_t;
}

namespace prism
{
class app_t;
}

namespace prism::acme
{
class challenge_store_t
{
public:
  void put(const std::string &token, std::string key_authorization);
  void remove(const std::string &token);
  std::optional<std::string> get(const std::string &token) const;

private:
  mutable std::mutex _mutex;
  std::unordered_map<std::string, std::string> _entries;
};

struct manager_config_t
{
  std::vector<std::string> domains;
  std::string contact_email;
  std::string directory_url;
  std::string storage_dir;
  std::optional<std::vector<uint8_t>> ca_mem;
  std::chrono::seconds renew_before = std::chrono::hours(24 * 30);
  std::chrono::seconds renewal_interval = std::chrono::hours(12);
  std::chrono::seconds renewal_backoff = std::chrono::hours(1);
  std::function<void(std::string_view)> log;
};

class manager_t
{
public:
  manager_t(vio::event_loop_t &loop, manager_config_t config);

  void install_challenge_route(app_t &http_app);
  vio::task_t<result_t<void>> ensure_certificates(std::shared_ptr<vio::sni_cert_store_t> sni_store, vio::cancellation_t *cancel = nullptr);
  vio::task_t<void> run_renewal_loop(std::shared_ptr<vio::sni_cert_store_t> sni_store, vio::cancellation_t *cancel = nullptr);

private:
  result_t<void> ensure_client();
  vio::task_t<result_t<void>> process_domain(const std::string &domain, std::shared_ptr<vio::sni_cert_store_t> sni_store, bool install_valid, vio::cancellation_t *cancel);
  void note(std::string_view message) const;

  std::string account_dir() const;
  std::string domain_dir(const std::string &domain) const;

  vio::event_loop_t &_loop;
  manager_config_t _config;
  std::shared_ptr<challenge_store_t> _challenge_store;
  std::unique_ptr<client_t> _client;
};
} // namespace prism::acme
