#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <vio/cancellation.h>
#include <vio/event_loop.h>
#include <vio/task.h>

#include "acme/jose.h"
#include "error.h"

namespace prism::web_push
{
struct subscription_t
{
  std::string endpoint;
  std::string p256dh;
  std::string auth;
};

struct message_t
{
  std::string payload;
  int ttl_seconds = 2419200;
};

class vapid_t
{
public:
  static result_t<vapid_t> generate(std::string subject);
  static result_t<vapid_t> from_pem(std::string_view private_pem, std::string subject);

  [[nodiscard]] result_t<std::string> private_pem() const;
  [[nodiscard]] const std::string &public_key_b64url() const
  {
    return _public_b64url;
  }
  [[nodiscard]] const std::string &subject() const
  {
    return _subject;
  }
  [[nodiscard]] const acme::ec_key_t &key() const
  {
    return _key;
  }

private:
  vapid_t(acme::ec_key_t key, std::string subject, std::string public_b64url);
  acme::ec_key_t _key;
  std::string _subject;
  std::string _public_b64url;
};

result_t<std::vector<std::uint8_t>> encrypt(const subscription_t &sub, std::string_view payload, const acme::ec_key_t &as_key, std::span<const std::uint8_t> salt);

result_t<std::string> vapid_authorization(const vapid_t &vapid, std::string_view endpoint, std::int64_t now_seconds);

vio::task_t<result_t<int>> send(vio::event_loop_t &loop, const vapid_t &vapid, const subscription_t &sub, const message_t &message, vio::cancellation_t *cancel = nullptr);
} // namespace prism::web_push
