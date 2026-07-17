#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <vio/cancellation.h>
#include <vio/event_loop.h>
#include <vio/operation/http_client.h>
#include <vio/task.h>

#include "../error.h"
#include "jose.h"

namespace prism::acme
{
struct issued_cert_t
{
  std::string fullchain_pem;
  std::string cert_key_pem;
};

using challenge_publish_t = std::function<void(const std::string &token, const std::string &key_authorization)>;
using challenge_cleanup_t = std::function<void(const std::string &token)>;

struct client_config_t
{
  std::string directory_url;
  std::string contact_email;
  std::optional<std::vector<uint8_t>> ca_mem;
  std::function<void(std::string_view)> log;
  std::chrono::milliseconds poll_interval{2000};
  int max_poll_attempts = 60;
};

class client_t
{
public:
  client_t(vio::event_loop_t &loop, client_config_t config, ec_key_t account_key);

  vio::task_t<result_t<issued_cert_t>> obtain_certificate(std::vector<std::string> domains, challenge_publish_t publish, challenge_cleanup_t cleanup, vio::cancellation_t *cancel = nullptr);

private:
  vio::task_t<result_t<void>> ensure_directory(vio::cancellation_t *cancel);
  vio::task_t<result_t<void>> refresh_nonce(vio::cancellation_t *cancel);
  vio::task_t<result_t<void>> ensure_account(vio::cancellation_t *cancel);
  vio::task_t<result_t<vio::http::response_t>> post_jws(const std::string &url, const std::string &payload, bool use_jwk, vio::cancellation_t *cancel);
  vio::task_t<result_t<vio::http::response_t>> post_as_get(const std::string &url, vio::cancellation_t *cancel);
  void note(std::string_view message) const;

  vio::event_loop_t &_loop;
  client_config_t _config;
  ec_key_t _account_key;
  std::string _new_nonce_url;
  std::string _new_account_url;
  std::string _new_order_url;
  std::string _nonce;
  std::string _kid;
  bool _have_directory = false;
};
} // namespace prism::acme
