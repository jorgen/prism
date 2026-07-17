#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../error.h"
#include "jose.h"

namespace prism::acme
{
result_t<std::vector<uint8_t>> make_csr_der(const ec_key_t &key, const std::vector<std::string> &dns_names);
result_t<int64_t> cert_not_after_unix(std::string_view fullchain_pem);
} // namespace prism::acme
