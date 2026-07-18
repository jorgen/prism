#include "web_push.h"

#include <array>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>

#include <vio/crypto.h>
#include <vio/operation/http_client.h>

namespace prism::web_push
{
namespace
{
std::vector<std::uint8_t> bytes_of(std::string_view text)
{
  return {reinterpret_cast<const std::uint8_t *>(text.data()), reinterpret_cast<const std::uint8_t *>(text.data()) + text.size()};
}

void append(std::vector<std::uint8_t> &out, std::span<const std::uint8_t> data)
{
  out.insert(out.end(), data.begin(), data.end());
}

std::vector<std::uint8_t> hkdf(std::span<const std::uint8_t> salt, std::span<const std::uint8_t> ikm, std::span<const std::uint8_t> info, std::size_t length)
{
  const vio::crypto::sha256_digest_t prk = vio::crypto::hmac_sha256(salt, ikm);
  std::vector<std::uint8_t> block(info.begin(), info.end());
  block.push_back(0x01);
  const vio::crypto::sha256_digest_t t = vio::crypto::hmac_sha256(std::span<const std::uint8_t>(prk), block);
  return std::vector<std::uint8_t>(t.begin(), t.begin() + length);
}

result_t<EVP_PKEY *> load_ec_public(std::span<const std::uint8_t> raw)
{
  EC_KEY *ec = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
  if (ec == nullptr)
  {
    return fail(status_t::internal_server_error, "web_push: EC_KEY_new failed");
  }
  const EC_GROUP *group = EC_KEY_get0_group(ec);
  EC_POINT *point = EC_POINT_new(group);
  bool ok = point != nullptr && EC_POINT_oct2point(group, point, raw.data(), raw.size(), nullptr) == 1 && EC_KEY_set_public_key(ec, point) == 1;
  if (point != nullptr)
  {
    EC_POINT_free(point);
  }
  if (!ok)
  {
    EC_KEY_free(ec);
    return fail(status_t::internal_server_error, "web_push: invalid subscription public key");
  }
  EVP_PKEY *pkey = EVP_PKEY_new();
  if (pkey == nullptr || EVP_PKEY_assign_EC_KEY(pkey, ec) != 1)
  {
    EC_KEY_free(ec);
    if (pkey != nullptr)
    {
      EVP_PKEY_free(pkey);
    }
    return fail(status_t::internal_server_error, "web_push: EVP_PKEY_assign failed");
  }
  return pkey;
}

result_t<std::vector<std::uint8_t>> ecdh_secret(EVP_PKEY *priv, EVP_PKEY *peer)
{
  EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(priv, nullptr);
  if (ctx == nullptr)
  {
    return fail(status_t::internal_server_error, "web_push: EVP_PKEY_CTX_new failed");
  }
  std::size_t len = 0;
  bool ok = EVP_PKEY_derive_init(ctx) == 1 && EVP_PKEY_derive_set_peer(ctx, peer) == 1 && EVP_PKEY_derive(ctx, nullptr, &len) == 1;
  std::vector<std::uint8_t> secret(len);
  ok = ok && EVP_PKEY_derive(ctx, secret.data(), &len) == 1;
  EVP_PKEY_CTX_free(ctx);
  if (!ok)
  {
    return fail(status_t::internal_server_error, "web_push: ECDH derive failed");
  }
  secret.resize(len);
  return secret;
}

result_t<std::vector<std::uint8_t>> aes128gcm_seal(std::span<const std::uint8_t> key, std::span<const std::uint8_t> nonce, std::span<const std::uint8_t> plaintext)
{
  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  if (ctx == nullptr)
  {
    return fail(status_t::internal_server_error, "web_push: EVP_CIPHER_CTX_new failed");
  }
  std::vector<std::uint8_t> out(plaintext.size());
  int outlen = 0;
  int finlen = 0;
  std::array<std::uint8_t, 16> tag{};
  bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_128_gcm(), nullptr, nullptr, nullptr) == 1 && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonce.size()), nullptr) == 1 &&
            EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce.data()) == 1 && EVP_EncryptUpdate(ctx, out.data(), &outlen, plaintext.data(), static_cast<int>(plaintext.size())) == 1 &&
            EVP_EncryptFinal_ex(ctx, out.data() + outlen, &finlen) == 1 && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, static_cast<int>(tag.size()), tag.data()) == 1;
  EVP_CIPHER_CTX_free(ctx);
  if (!ok)
  {
    return fail(status_t::internal_server_error, "web_push: AES-128-GCM encryption failed");
  }
  out.resize(static_cast<std::size_t>(outlen + finlen));
  append(out, tag);
  return out;
}

std::string origin_of(std::string_view url)
{
  const auto scheme = url.find("://");
  if (scheme == std::string_view::npos)
  {
    return {};
  }
  const auto path = url.find('/', scheme + 3);
  return std::string(path == std::string_view::npos ? url : url.substr(0, path));
}
} // namespace

vapid_t::vapid_t(acme::ec_key_t key, std::string subject, std::string public_b64url)
  : _key(std::move(key))
  , _subject(std::move(subject))
  , _public_b64url(std::move(public_b64url))
{
}

result_t<vapid_t> vapid_t::from_pem(std::string_view private_pem, std::string subject)
{
  auto key = acme::ec_key_t::from_pem(private_pem);
  if (!key)
  {
    return std::unexpected(key.error());
  }
  auto raw = acme::public_key_raw(*key);
  if (!raw)
  {
    return std::unexpected(raw.error());
  }
  return vapid_t(std::move(*key), std::move(subject), vio::crypto::base64url_encode(*raw));
}

result_t<vapid_t> vapid_t::generate(std::string subject)
{
  auto key = acme::ec_key_t::generate();
  if (!key)
  {
    return std::unexpected(key.error());
  }
  auto raw = acme::public_key_raw(*key);
  if (!raw)
  {
    return std::unexpected(raw.error());
  }
  return vapid_t(std::move(*key), std::move(subject), vio::crypto::base64url_encode(*raw));
}

result_t<std::string> vapid_t::private_pem() const
{
  return _key.to_pem();
}

result_t<std::vector<std::uint8_t>> encrypt(const subscription_t &sub, std::string_view payload, const acme::ec_key_t &as_key, std::span<const std::uint8_t> salt)
{
  if (salt.size() != 16)
  {
    return fail(status_t::internal_server_error, "web_push: salt must be 16 bytes");
  }
  auto ua_public = vio::crypto::base64url_decode(sub.p256dh);
  if (!ua_public || ua_public->size() != 65)
  {
    return fail(status_t::bad_request, "web_push: invalid p256dh");
  }
  auto auth = vio::crypto::base64url_decode(sub.auth);
  if (!auth || auth->size() != 16)
  {
    return fail(status_t::bad_request, "web_push: invalid auth secret");
  }
  auto as_public = acme::public_key_raw(as_key);
  if (!as_public)
  {
    return std::unexpected(as_public.error());
  }

  auto ua_pkey = load_ec_public(*ua_public);
  if (!ua_pkey)
  {
    return std::unexpected(ua_pkey.error());
  }
  auto shared = ecdh_secret(as_key.pkey(), *ua_pkey);
  EVP_PKEY_free(*ua_pkey);
  if (!shared)
  {
    return std::unexpected(shared.error());
  }

  std::vector<std::uint8_t> key_info = bytes_of("WebPush: info");
  key_info.push_back(0x00);
  append(key_info, *ua_public);
  append(key_info, *as_public);
  const std::vector<std::uint8_t> ikm = hkdf(*auth, *shared, key_info, 32);

  std::vector<std::uint8_t> cek_info = bytes_of("Content-Encoding: aes128gcm");
  cek_info.push_back(0x00);
  std::vector<std::uint8_t> nonce_info = bytes_of("Content-Encoding: nonce");
  nonce_info.push_back(0x00);
  const std::vector<std::uint8_t> cek = hkdf(salt, ikm, cek_info, 16);
  const std::vector<std::uint8_t> nonce = hkdf(salt, ikm, nonce_info, 12);

  std::vector<std::uint8_t> plaintext(payload.begin(), payload.end());
  plaintext.push_back(0x02);
  auto ciphertext = aes128gcm_seal(cek, nonce, plaintext);
  if (!ciphertext)
  {
    return std::unexpected(ciphertext.error());
  }

  constexpr std::uint32_t record_size = 4096;
  std::vector<std::uint8_t> body;
  body.reserve(salt.size() + 4 + 1 + as_public->size() + ciphertext->size());
  append(body, salt);
  body.push_back(static_cast<std::uint8_t>((record_size >> 24) & 0xff));
  body.push_back(static_cast<std::uint8_t>((record_size >> 16) & 0xff));
  body.push_back(static_cast<std::uint8_t>((record_size >> 8) & 0xff));
  body.push_back(static_cast<std::uint8_t>(record_size & 0xff));
  body.push_back(static_cast<std::uint8_t>(as_public->size()));
  append(body, *as_public);
  append(body, *ciphertext);
  return body;
}

result_t<std::string> vapid_authorization(const vapid_t &vapid, std::string_view endpoint, std::int64_t now_seconds)
{
  const std::string aud = origin_of(endpoint);
  if (aud.empty())
  {
    return fail(status_t::bad_request, "web_push: invalid endpoint");
  }
  const std::int64_t exp = now_seconds + 12 * 60 * 60;
  const std::string header = R"({"typ":"JWT","alg":"ES256"})";
  const std::string claims = std::string(R"({"aud":")") + aud + R"(","exp":)" + std::to_string(exp) + R"(,"sub":")" + vapid.subject() + R"("})";
  auto jwt = acme::es256_compact(vapid.key(), header, claims);
  if (!jwt)
  {
    return std::unexpected(jwt.error());
  }
  return std::string("vapid t=") + *jwt + ", k=" + vapid.public_key_b64url();
}

vio::task_t<result_t<int>> send(vio::event_loop_t &loop, const vapid_t &vapid, const subscription_t &sub, const message_t &message, vio::cancellation_t *cancel)
{
  auto as_key = acme::ec_key_t::generate();
  if (!as_key)
  {
    co_return std::unexpected(as_key.error());
  }
  std::array<std::uint8_t, 16> salt{};
  if (!vio::crypto::random_bytes(salt))
  {
    co_return fail(status_t::internal_server_error, "web_push: rng failed");
  }
  auto body = encrypt(sub, message.payload, *as_key, salt);
  if (!body)
  {
    co_return std::unexpected(body.error());
  }
  auto authorization = vapid_authorization(vapid, sub.endpoint, static_cast<std::int64_t>(std::time(nullptr)));
  if (!authorization)
  {
    co_return std::unexpected(authorization.error());
  }

  vio::http::request_t request;
  request.method = "POST";
  request.url = sub.endpoint;
  request.headers.push_back(vio::http::header_t{"Authorization", *authorization});
  request.headers.push_back(vio::http::header_t{"Content-Encoding", "aes128gcm"});
  request.headers.push_back(vio::http::header_t{"TTL", std::to_string(message.ttl_seconds)});
  request.headers.push_back(vio::http::header_t{"Content-Type", "application/octet-stream"});
  request.body.assign(body->begin(), body->end());

  auto response = co_await vio::http::fetch(loop, request, cancel);
  if (!response)
  {
    co_return fail(status_t::bad_gateway, std::string("web_push: send failed: ") + response.error().msg);
  }
  co_return response->status;
}
} // namespace prism::web_push
