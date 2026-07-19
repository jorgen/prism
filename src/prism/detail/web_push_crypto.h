#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../acme/jose.h"
#include "../error.h"
#include "../web_push.h"

namespace prism::web_push::detail
{
result_t<std::vector<std::uint8_t>> encrypt(const subscription_t &sub, std::string_view payload, const acme::ec_key_t &as_key, std::span<const std::uint8_t> salt);

result_t<std::string> vapid_authorization(const vapid_t &vapid, std::string_view endpoint, std::int64_t now_seconds);
} // namespace prism::web_push::detail
