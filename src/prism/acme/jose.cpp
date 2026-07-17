#include "jose.h"

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/pem.h>

#include <vio/crypto.h>

namespace prism::acme
{
namespace
{
constexpr int coordinate_bytes = 32;

result_t<std::pair<std::string, std::string>> public_coordinates(EVP_PKEY *pkey)
{
  EC_KEY *ec = EVP_PKEY_get0_EC_KEY(pkey);
  if (ec == nullptr)
  {
    return fail(status_t::internal_server_error, "acme: key is not EC");
  }
  const EC_GROUP *group = EC_KEY_get0_group(ec);
  const EC_POINT *point = EC_KEY_get0_public_key(ec);
  if (group == nullptr || point == nullptr)
  {
    return fail(status_t::internal_server_error, "acme: EC public key unavailable");
  }
  BIGNUM *x = BN_new();
  BIGNUM *y = BN_new();
  std::pair<std::string, std::string> out;
  bool ok = x != nullptr && y != nullptr && EC_POINT_get_affine_coordinates(group, point, x, y, nullptr) == 1;
  if (ok)
  {
    std::array<uint8_t, coordinate_bytes> xb{};
    std::array<uint8_t, coordinate_bytes> yb{};
    ok = BN_bn2binpad(x, xb.data(), coordinate_bytes) == coordinate_bytes && BN_bn2binpad(y, yb.data(), coordinate_bytes) == coordinate_bytes;
    if (ok)
    {
      out.first = vio::crypto::base64url_encode(xb);
      out.second = vio::crypto::base64url_encode(yb);
    }
  }
  BN_free(x);
  BN_free(y);
  if (!ok)
  {
    return fail(status_t::internal_server_error, "acme: failed to read EC coordinates");
  }
  return out;
}

std::vector<uint8_t> as_bytes(std::string_view text)
{
  return {reinterpret_cast<const uint8_t *>(text.data()), reinterpret_cast<const uint8_t *>(text.data()) + text.size()};
}
} // namespace

ec_key_t::ec_key_t(ec_key_t &&other) noexcept
  : _pkey(other._pkey)
{
  other._pkey = nullptr;
}

ec_key_t &ec_key_t::operator=(ec_key_t &&other) noexcept
{
  if (this != &other)
  {
    if (_pkey != nullptr)
    {
      EVP_PKEY_free(_pkey);
    }
    _pkey = other._pkey;
    other._pkey = nullptr;
  }
  return *this;
}

ec_key_t::~ec_key_t()
{
  if (_pkey != nullptr)
  {
    EVP_PKEY_free(_pkey);
  }
}

result_t<ec_key_t> ec_key_t::generate()
{
  EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
  if (ctx == nullptr)
  {
    return fail(status_t::internal_server_error, "acme: EVP_PKEY_CTX_new_id failed");
  }
  EVP_PKEY *pkey = nullptr;
  bool ok = EVP_PKEY_keygen_init(ctx) == 1 && EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx, NID_X9_62_prime256v1) == 1 && EVP_PKEY_keygen(ctx, &pkey) == 1;
  EVP_PKEY_CTX_free(ctx);
  if (!ok || pkey == nullptr)
  {
    if (pkey != nullptr)
    {
      EVP_PKEY_free(pkey);
    }
    return fail(status_t::internal_server_error, "acme: P-256 key generation failed");
  }
  return ec_key_t(pkey);
}

result_t<ec_key_t> ec_key_t::from_pem(std::string_view pem)
{
  BIO *bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
  if (bio == nullptr)
  {
    return fail(status_t::internal_server_error, "acme: BIO_new_mem_buf failed");
  }
  EVP_PKEY *pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  if (pkey == nullptr)
  {
    return fail(status_t::internal_server_error, "acme: failed to parse private key PEM");
  }
  return ec_key_t(pkey);
}

result_t<std::string> ec_key_t::to_pem() const
{
  if (_pkey == nullptr)
  {
    return fail(status_t::internal_server_error, "acme: no key to serialize");
  }
  BIO *bio = BIO_new(BIO_s_mem());
  if (bio == nullptr)
  {
    return fail(status_t::internal_server_error, "acme: BIO_new failed");
  }
  std::string out;
  if (PEM_write_bio_PrivateKey(bio, _pkey, nullptr, nullptr, 0, nullptr, nullptr) == 1)
  {
    char *data = nullptr;
    long len = BIO_get_mem_data(bio, &data);
    out.assign(data, static_cast<size_t>(len));
  }
  BIO_free(bio);
  if (out.empty())
  {
    return fail(status_t::internal_server_error, "acme: failed to serialize private key");
  }
  return out;
}

result_t<std::string> jwk_json(const ec_key_t &key)
{
  auto coords = public_coordinates(key.pkey());
  if (!coords)
  {
    return std::unexpected(coords.error());
  }
  return std::string("{\"crv\":\"P-256\",\"kty\":\"EC\",\"x\":\"") + coords->first + "\",\"y\":\"" + coords->second + "\"}";
}

result_t<std::string> jwk_thumbprint(const ec_key_t &key)
{
  auto jwk = jwk_json(key);
  if (!jwk)
  {
    return std::unexpected(jwk.error());
  }
  auto digest = vio::crypto::sha256(as_bytes(*jwk));
  return vio::crypto::base64url_encode(digest);
}

result_t<std::string> es256_jws(const ec_key_t &key, std::string_view protected_json, std::string_view payload)
{
  if (!key)
  {
    return fail(status_t::internal_server_error, "acme: signing key is empty");
  }
  const std::string protected_b64 = vio::crypto::base64url_encode(as_bytes(protected_json));
  const std::string payload_b64 = vio::crypto::base64url_encode(as_bytes(payload));
  const std::string signing_input = protected_b64 + "." + payload_b64;

  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  if (ctx == nullptr)
  {
    return fail(status_t::internal_server_error, "acme: EVP_MD_CTX_new failed");
  }
  std::vector<uint8_t> der;
  bool ok = EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, key.pkey()) == 1 && EVP_DigestSignUpdate(ctx, signing_input.data(), signing_input.size()) == 1;
  size_t der_len = 0;
  ok = ok && EVP_DigestSignFinal(ctx, nullptr, &der_len) == 1;
  if (ok)
  {
    der.resize(der_len);
    ok = EVP_DigestSignFinal(ctx, der.data(), &der_len) == 1;
    der.resize(der_len);
  }
  EVP_MD_CTX_free(ctx);
  if (!ok)
  {
    return fail(status_t::internal_server_error, "acme: ES256 signing failed");
  }

  const uint8_t *der_ptr = der.data();
  ECDSA_SIG *sig = d2i_ECDSA_SIG(nullptr, &der_ptr, static_cast<long>(der.size()));
  if (sig == nullptr)
  {
    return fail(status_t::internal_server_error, "acme: failed to decode ECDSA signature");
  }
  const BIGNUM *r = nullptr;
  const BIGNUM *s = nullptr;
  ECDSA_SIG_get0(sig, &r, &s);
  std::array<uint8_t, 2 * coordinate_bytes> raw{};
  const bool raw_ok = BN_bn2binpad(r, raw.data(), coordinate_bytes) == coordinate_bytes && BN_bn2binpad(s, raw.data() + coordinate_bytes, coordinate_bytes) == coordinate_bytes;
  ECDSA_SIG_free(sig);
  if (!raw_ok)
  {
    return fail(status_t::internal_server_error, "acme: failed to encode ECDSA signature");
  }
  const std::string signature_b64 = vio::crypto::base64url_encode(raw);
  return std::string("{\"protected\":\"") + protected_b64 + "\",\"payload\":\"" + payload_b64 + "\",\"signature\":\"" + signature_b64 + "\"}";
}

std::string http01_key_authorization(std::string_view token, std::string_view thumbprint)
{
  std::string out;
  out.reserve(token.size() + 1 + thumbprint.size());
  out.append(token);
  out.push_back('.');
  out.append(thumbprint);
  return out;
}
} // namespace prism::acme
