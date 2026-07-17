#pragma once

#include <string>
#include <string_view>

#include <openssl/evp.h>

#include "../error.h"

namespace prism::acme
{
class ec_key_t
{
public:
  ec_key_t() = default;
  ec_key_t(const ec_key_t &) = delete;
  ec_key_t &operator=(const ec_key_t &) = delete;
  ec_key_t(ec_key_t &&other) noexcept;
  ec_key_t &operator=(ec_key_t &&other) noexcept;
  ~ec_key_t();

  static result_t<ec_key_t> generate();
  static result_t<ec_key_t> from_pem(std::string_view pem);
  result_t<std::string> to_pem() const;

  EVP_PKEY *pkey() const
  {
    return _pkey;
  }
  explicit operator bool() const
  {
    return _pkey != nullptr;
  }

private:
  explicit ec_key_t(EVP_PKEY *pkey)
    : _pkey(pkey)
  {
  }
  EVP_PKEY *_pkey = nullptr;
};

result_t<std::string> jwk_json(const ec_key_t &key);
result_t<std::string> jwk_thumbprint(const ec_key_t &key);
result_t<std::string> es256_jws(const ec_key_t &key, std::string_view protected_json, std::string_view payload);
std::string http01_key_authorization(std::string_view token, std::string_view thumbprint);
} // namespace prism::acme
