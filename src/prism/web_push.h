#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

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

enum class delivery_t : std::uint8_t
{
  delivered,
  gone,
  rejected,
  unavailable,
};

struct send_result_t
{
  delivery_t delivery = delivery_t::unavailable;
  int status = 0;

  [[nodiscard]] bool delivered() const
  {
    return delivery == delivery_t::delivered;
  }
  [[nodiscard]] bool gone() const
  {
    return delivery == delivery_t::gone;
  }
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

vio::task_t<send_result_t> send(vio::event_loop_t &loop, const vapid_t &vapid, const subscription_t &sub, const message_t &message, std::chrono::milliseconds timeout = std::chrono::seconds{10},
                                vio::cancellation_t *cancel = nullptr);
} // namespace prism::web_push
