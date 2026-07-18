#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>

#include <prism/acme/jose.h>
#include <prism/web_push.h>

#include <vio/crypto.h>

namespace
{
namespace wp = prism::web_push;

std::vector<std::uint8_t> hkdf(std::span<const std::uint8_t> salt, std::span<const std::uint8_t> ikm, std::span<const std::uint8_t> info, std::size_t length)
{
  const vio::crypto::sha256_digest_t prk = vio::crypto::hmac_sha256(salt, ikm);
  std::vector<std::uint8_t> block(info.begin(), info.end());
  block.push_back(0x01);
  const vio::crypto::sha256_digest_t t = vio::crypto::hmac_sha256(std::span<const std::uint8_t>(prk), block);
  return std::vector<std::uint8_t>(t.begin(), t.begin() + length);
}

std::vector<std::uint8_t> info_with_null(std::string_view text)
{
  std::vector<std::uint8_t> out(text.begin(), text.end());
  out.push_back(0x00);
  return out;
}

EVP_PKEY *load_public(std::span<const std::uint8_t> raw)
{
  EC_KEY *ec = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
  const EC_GROUP *group = EC_KEY_get0_group(ec);
  EC_POINT *point = EC_POINT_new(group);
  EC_POINT_oct2point(group, point, raw.data(), raw.size(), nullptr);
  EC_KEY_set_public_key(ec, point);
  EC_POINT_free(point);
  EVP_PKEY *pkey = EVP_PKEY_new();
  EVP_PKEY_assign_EC_KEY(pkey, ec);
  return pkey;
}

std::vector<std::uint8_t> ecdh(EVP_PKEY *priv, EVP_PKEY *peer)
{
  EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(priv, nullptr);
  std::size_t len = 0;
  EVP_PKEY_derive_init(ctx);
  EVP_PKEY_derive_set_peer(ctx, peer);
  EVP_PKEY_derive(ctx, nullptr, &len);
  std::vector<std::uint8_t> secret(len);
  EVP_PKEY_derive(ctx, secret.data(), &len);
  EVP_PKEY_CTX_free(ctx);
  secret.resize(len);
  return secret;
}

std::optional<std::vector<std::uint8_t>> gcm_open(std::span<const std::uint8_t> key, std::span<const std::uint8_t> nonce, std::span<const std::uint8_t> ct_and_tag)
{
  if (ct_and_tag.size() < 16)
  {
    return std::nullopt;
  }
  const std::size_t ctlen = ct_and_tag.size() - 16;
  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  std::vector<std::uint8_t> out(ctlen);
  int outlen = 0;
  int finlen = 0;
  bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_128_gcm(), nullptr, nullptr, nullptr) == 1 && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonce.size()), nullptr) == 1 &&
            EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce.data()) == 1 && EVP_DecryptUpdate(ctx, out.data(), &outlen, ct_and_tag.data(), static_cast<int>(ctlen)) == 1 &&
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, const_cast<std::uint8_t *>(ct_and_tag.data() + ctlen)) == 1 && EVP_DecryptFinal_ex(ctx, out.data() + outlen, &finlen) == 1;
  EVP_CIPHER_CTX_free(ctx);
  if (!ok)
  {
    return std::nullopt;
  }
  out.resize(static_cast<std::size_t>(outlen + finlen));
  return out;
}

std::optional<std::string> decrypt(std::span<const std::uint8_t> body, EVP_PKEY *ua_key, std::span<const std::uint8_t> ua_public, std::span<const std::uint8_t> auth)
{
  if (body.size() < 21)
  {
    return std::nullopt;
  }
  const std::span<const std::uint8_t> salt = body.subspan(0, 16);
  const std::uint8_t idlen = body[20];
  if (body.size() < 21u + idlen)
  {
    return std::nullopt;
  }
  const std::span<const std::uint8_t> as_public = body.subspan(21, idlen);
  const std::span<const std::uint8_t> ciphertext = body.subspan(21u + idlen);

  EVP_PKEY *as_pkey = load_public(as_public);
  const std::vector<std::uint8_t> shared = ecdh(ua_key, as_pkey);
  EVP_PKEY_free(as_pkey);

  std::vector<std::uint8_t> key_info(info_with_null("WebPush: info"));
  key_info.insert(key_info.end(), ua_public.begin(), ua_public.end());
  key_info.insert(key_info.end(), as_public.begin(), as_public.end());
  const std::vector<std::uint8_t> ikm = hkdf(auth, shared, key_info, 32);
  const std::vector<std::uint8_t> cek = hkdf(salt, ikm, info_with_null("Content-Encoding: aes128gcm"), 16);
  const std::vector<std::uint8_t> nonce = hkdf(salt, ikm, info_with_null("Content-Encoding: nonce"), 12);

  auto plain = gcm_open(cek, nonce, ciphertext);
  if (!plain)
  {
    return std::nullopt;
  }
  while (!plain->empty() && plain->back() == 0x00)
  {
    plain->pop_back();
  }
  if (plain->empty() || plain->back() != 0x02)
  {
    return std::nullopt;
  }
  plain->pop_back();
  return std::string(plain->begin(), plain->end());
}
} // namespace

TEST_CASE("web_push: RFC 8291 aes128gcm payload round-trips through a real recipient key")
{
  auto ua = prism::acme::ec_key_t::generate();
  REQUIRE(ua.has_value());
  auto ua_public = prism::acme::public_key_raw(*ua);
  REQUIRE(ua_public.has_value());
  std::array<std::uint8_t, 16> auth{};
  REQUIRE(vio::crypto::random_bytes(auth).has_value());

  wp::subscription_t sub;
  sub.endpoint = "https://push.example.com/abc";
  sub.p256dh = vio::crypto::base64url_encode(*ua_public);
  sub.auth = vio::crypto::base64url_encode(auth);

  auto as_key = prism::acme::ec_key_t::generate();
  REQUIRE(as_key.has_value());
  std::array<std::uint8_t, 16> salt{};
  REQUIRE(vio::crypto::random_bytes(salt).has_value());

  const std::string payload = "{\"type\":\"changed\",\"listId\":\"abc-123\"}";
  auto body = wp::encrypt(sub, payload, *as_key, salt);
  REQUIRE(body.has_value());

  CHECK(body->size() == 16 + 4 + 1 + 65 + (payload.size() + 1 + 16));
  CHECK((*body)[20] == 65);
  CHECK((*body)[16] == 0x00);
  CHECK((*body)[18] == 0x10);
  CHECK((*body)[19] == 0x00);

  auto recovered = decrypt(*body, ua->pkey(), *ua_public, auth);
  REQUIRE(recovered.has_value());
  CHECK(*recovered == payload);
}

TEST_CASE("web_push: a different recipient cannot decrypt")
{
  auto ua = prism::acme::ec_key_t::generate();
  auto ua_public = prism::acme::public_key_raw(*ua);
  std::array<std::uint8_t, 16> auth{};
  vio::crypto::random_bytes(auth);
  wp::subscription_t sub{"https://push.example.com/abc", vio::crypto::base64url_encode(*ua_public), vio::crypto::base64url_encode(auth)};
  auto as_key = prism::acme::ec_key_t::generate();
  std::array<std::uint8_t, 16> salt{};
  vio::crypto::random_bytes(salt);
  auto body = wp::encrypt(sub, "secret", *as_key, salt);
  REQUIRE(body.has_value());

  auto other = prism::acme::ec_key_t::generate();
  auto other_public = prism::acme::public_key_raw(*other);
  auto recovered = decrypt(*body, other->pkey(), *other_public, auth);
  CHECK_FALSE(recovered.has_value());
}

TEST_CASE("web_push: VAPID authorization is a compact ES256 JWT with the right audience")
{
  auto vapid = wp::vapid_t::generate("mailto:ops@example.com");
  REQUIRE(vapid.has_value());
  auto authorization = wp::vapid_authorization(*vapid, "https://fcm.googleapis.com/fcm/send/xyz", 1700000000);
  REQUIRE(authorization.has_value());

  CHECK(authorization->rfind("vapid t=", 0) == 0);
  const auto k = authorization->find(", k=");
  REQUIRE(k != std::string::npos);
  const std::string jwt = authorization->substr(8, k - 8);
  const std::string k_param = authorization->substr(k + 4);
  CHECK(k_param == vapid->public_key_b64url());

  const auto dot1 = jwt.find('.');
  const auto dot2 = jwt.find('.', dot1 + 1);
  REQUIRE(dot1 != std::string::npos);
  REQUIRE(dot2 != std::string::npos);

  auto header = vio::crypto::base64url_decode(jwt.substr(0, dot1));
  auto claims = vio::crypto::base64url_decode(jwt.substr(dot1 + 1, dot2 - dot1 - 1));
  REQUIRE(header.has_value());
  REQUIRE(claims.has_value());
  const std::string header_json(header->begin(), header->end());
  const std::string claims_json(claims->begin(), claims->end());
  CHECK(header_json.find("\"ES256\"") != std::string::npos);
  CHECK(claims_json.find("\"aud\":\"https://fcm.googleapis.com\"") != std::string::npos);
  CHECK(claims_json.find("\"sub\":\"mailto:ops@example.com\"") != std::string::npos);
  CHECK(claims_json.find("\"exp\":1700043200") != std::string::npos);
}
