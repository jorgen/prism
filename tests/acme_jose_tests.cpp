#include <doctest/doctest.h>

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <vio/crypto.h>

#include <prism/acme/csr.h>
#include <prism/acme/jose.h>

namespace
{
const char *const test_key_pem =
  "-----BEGIN EC PRIVATE KEY-----\n"
  "MHcCAQEEIIVGdyEIxSLLpc7EG2/ShWuMYlmt3TYLl0OiMXhLYRr7oAoGCCqGSM49\n"
  "AwEHoUQDQgAEwx2kwiAKVSZKVMqwblRMdZFIe3Gk5AfT/rpkP/wu3ArL/0KyKqec\n"
  "0MjNAXZtz8kVq3nVu5EIxexCnsmJrCugNQ==\n"
  "-----END EC PRIVATE KEY-----\n";

std::string json_string_field(const std::string &json, const std::string &key)
{
  const std::string needle = "\"" + key + "\":\"";
  const auto start = json.find(needle);
  if (start == std::string::npos)
  {
    return {};
  }
  const auto value_start = start + needle.size();
  const auto value_end = json.find('"', value_start);
  if (value_end == std::string::npos)
  {
    return {};
  }
  return json.substr(value_start, value_end - value_start);
}

std::string decode_utf8(const std::string &b64url)
{
  auto bytes = vio::crypto::base64url_decode(b64url).value();
  return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}
} // namespace

TEST_SUITE("acme jose")
{
  TEST_CASE("jwk_json and RFC 7638 thumbprint match an independent computation")
  {
    auto key = prism::acme::ec_key_t::from_pem(test_key_pem);
    REQUIRE(key.has_value());

    auto jwk = prism::acme::jwk_json(*key);
    REQUIRE(jwk.has_value());
    CHECK(*jwk == "{\"crv\":\"P-256\",\"kty\":\"EC\",\"x\":\"wx2kwiAKVSZKVMqwblRMdZFIe3Gk5AfT_rpkP_wu3Ao\",\"y\":\"y_9CsiqnnNDIzQF2bc_JFat51buRCMXsQp7JiawroDU\"}");

    auto thumbprint = prism::acme::jwk_thumbprint(*key);
    REQUIRE(thumbprint.has_value());
    CHECK(*thumbprint == "R-dlg8syGMTW_Q60-JwMqeCKpIt0zPjVue3iqrFxOIk");
  }

  TEST_CASE("es256_jws produces a flattened JWS whose signature verifies")
  {
    auto key = prism::acme::ec_key_t::generate();
    REQUIRE(key.has_value());

    const std::string protected_json = "{\"alg\":\"ES256\",\"nonce\":\"abc\",\"url\":\"https://acme.example/new-order\"}";
    const std::string payload = "{\"identifiers\":[{\"type\":\"dns\",\"value\":\"host.example\"}]}";

    auto jws = prism::acme::es256_jws(*key, protected_json, payload);
    REQUIRE(jws.has_value());

    const std::string protected_b64 = json_string_field(*jws, "protected");
    const std::string payload_b64 = json_string_field(*jws, "payload");
    const std::string signature_b64 = json_string_field(*jws, "signature");
    REQUIRE_FALSE(protected_b64.empty());
    REQUIRE_FALSE(payload_b64.empty());
    REQUIRE_FALSE(signature_b64.empty());

    CHECK(decode_utf8(protected_b64) == protected_json);
    CHECK(decode_utf8(payload_b64) == payload);

    auto raw = vio::crypto::base64url_decode(signature_b64);
    REQUIRE(raw.has_value());
    REQUIRE(raw->size() == 64);

    ECDSA_SIG *sig = ECDSA_SIG_new();
    BIGNUM *r = BN_bin2bn(raw->data(), 32, nullptr);
    BIGNUM *s = BN_bin2bn(raw->data() + 32, 32, nullptr);
    REQUIRE(ECDSA_SIG_set0(sig, r, s) == 1);
    const int der_len = i2d_ECDSA_SIG(sig, nullptr);
    REQUIRE(der_len > 0);
    std::vector<uint8_t> der(static_cast<size_t>(der_len));
    uint8_t *der_ptr = der.data();
    i2d_ECDSA_SIG(sig, &der_ptr);
    ECDSA_SIG_free(sig);

    const std::string signing_input = protected_b64 + "." + payload_b64;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    REQUIRE(EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, key->pkey()) == 1);
    REQUIRE(EVP_DigestVerifyUpdate(ctx, signing_input.data(), signing_input.size()) == 1);
    const int verified = EVP_DigestVerifyFinal(ctx, der.data(), der.size());
    EVP_MD_CTX_free(ctx);
    CHECK(verified == 1);
  }

  TEST_CASE("http01_key_authorization joins token and thumbprint with a dot")
  {
    CHECK(prism::acme::http01_key_authorization("tok123", "thumb456") == "tok123.thumb456");
  }

  TEST_CASE("make_csr_der yields a self-consistent CSR carrying every DNS SAN")
  {
    auto key = prism::acme::ec_key_t::generate();
    REQUIRE(key.has_value());

    const std::vector<std::string> names{"a.example.com", "b.example.com"};
    auto der = prism::acme::make_csr_der(*key, names);
    REQUIRE(der.has_value());

    const uint8_t *der_ptr = der->data();
    X509_REQ *req = d2i_X509_REQ(nullptr, &der_ptr, static_cast<long>(der->size()));
    REQUIRE(req != nullptr);

    EVP_PKEY *pub = X509_REQ_get_pubkey(req);
    REQUIRE(pub != nullptr);
    CHECK(X509_REQ_verify(req, pub) == 1);
    EVP_PKEY_free(pub);

    STACK_OF(X509_EXTENSION) *exts = X509_REQ_get_extensions(req);
    REQUIRE(exts != nullptr);
    const int idx = X509v3_get_ext_by_NID(exts, NID_subject_alt_name, -1);
    REQUIRE(idx >= 0);
    X509_EXTENSION *ext = X509v3_get_ext(exts, idx);
    auto *san = static_cast<GENERAL_NAMES *>(X509V3_EXT_d2i(ext));
    REQUIRE(san != nullptr);

    std::vector<std::string> found;
    for (int i = 0; i < sk_GENERAL_NAME_num(san); ++i)
    {
      GENERAL_NAME *gn = sk_GENERAL_NAME_value(san, i);
      int type = 0;
      auto *value = static_cast<ASN1_STRING *>(GENERAL_NAME_get0_value(gn, &type));
      if (type == GEN_DNS && value != nullptr)
      {
        found.emplace_back(reinterpret_cast<const char *>(ASN1_STRING_get0_data(value)), static_cast<size_t>(ASN1_STRING_length(value)));
      }
    }
    GENERAL_NAMES_free(san);
    sk_X509_EXTENSION_pop_free(exts, X509_EXTENSION_free);
    X509_REQ_free(req);

    CHECK(found == names);
  }

  TEST_CASE("cert_not_after_unix reads the leaf notAfter")
  {
    auto key = prism::acme::ec_key_t::generate();
    REQUIRE(key.has_value());

    const int64_t fixed_not_after = 2000000000;
    X509 *cert = X509_new();
    X509_set_version(cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
    X509_gmtime_adj(X509_getm_notBefore(cert), 0);
    ASN1_TIME_set(X509_getm_notAfter(cert), static_cast<time_t>(fixed_not_after));
    X509_set_pubkey(cert, key->pkey());
    X509_NAME *name = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char *>("host.example"), -1, -1, 0);
    X509_set_issuer_name(cert, name);
    REQUIRE(X509_sign(cert, key->pkey(), EVP_sha256()) > 0);

    BIO *bio = BIO_new(BIO_s_mem());
    REQUIRE(PEM_write_bio_X509(bio, cert) == 1);
    char *data = nullptr;
    const long len = BIO_get_mem_data(bio, &data);
    const std::string pem(data, static_cast<size_t>(len));
    BIO_free(bio);
    X509_free(cert);

    auto not_after = prism::acme::cert_not_after_unix(pem);
    REQUIRE(not_after.has_value());
    CHECK(*not_after == fixed_not_after);
  }
}
