#include "csr.h"

#include <ctime>

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

namespace prism::acme
{
namespace
{
int64_t tm_to_unix(const struct tm &value)
{
  struct tm copy = value;
#if defined(_WIN32)
  return static_cast<int64_t>(_mkgmtime(&copy));
#else
  return static_cast<int64_t>(timegm(&copy));
#endif
}
} // namespace

result_t<std::vector<uint8_t>> make_csr_der(const ec_key_t &key, const std::vector<std::string> &dns_names)
{
  if (!key)
  {
    return fail(status_t::internal_server_error, "acme: CSR key is empty");
  }
  if (dns_names.empty())
  {
    return fail(status_t::internal_server_error, "acme: CSR needs at least one DNS name");
  }

  X509_REQ *req = X509_REQ_new();
  GENERAL_NAMES *sans = sk_GENERAL_NAME_new_null();
  STACK_OF(X509_EXTENSION) *exts = sk_X509_EXTENSION_new_null();
  X509_EXTENSION *san_ext = nullptr;
  std::vector<uint8_t> der;
  bool ok = req != nullptr && sans != nullptr && exts != nullptr;

  if (ok)
  {
    ok = X509_REQ_set_version(req, 0) == 1 && X509_REQ_set_pubkey(req, key.pkey()) == 1;
  }
  for (size_t i = 0; ok && i < dns_names.size(); ++i)
  {
    GENERAL_NAME *gn = GENERAL_NAME_new();
    ASN1_IA5STRING *ia5 = ASN1_IA5STRING_new();
    if (gn == nullptr || ia5 == nullptr || ASN1_STRING_set(ia5, dns_names[i].data(), static_cast<int>(dns_names[i].size())) != 1)
    {
      if (ia5 != nullptr)
      {
        ASN1_IA5STRING_free(ia5);
      }
      if (gn != nullptr)
      {
        GENERAL_NAME_free(gn);
      }
      ok = false;
      break;
    }
    GENERAL_NAME_set0_value(gn, GEN_DNS, ia5);
    if (sk_GENERAL_NAME_push(sans, gn) == 0)
    {
      GENERAL_NAME_free(gn);
      ok = false;
    }
  }
  if (ok)
  {
    san_ext = X509V3_EXT_i2d(NID_subject_alt_name, 0, sans);
    ok = san_ext != nullptr && sk_X509_EXTENSION_push(exts, san_ext) != 0;
  }
  if (ok)
  {
    ok = X509_REQ_add_extensions(req, exts) == 1 && X509_REQ_sign(req, key.pkey(), EVP_sha256()) > 0;
  }
  if (ok)
  {
    const int len = i2d_X509_REQ(req, nullptr);
    if (len > 0)
    {
      der.resize(static_cast<size_t>(len));
      uint8_t *out = der.data();
      ok = i2d_X509_REQ(req, &out) == len;
    }
    else
    {
      ok = false;
    }
  }

  GENERAL_NAMES_free(sans);
  sk_X509_EXTENSION_pop_free(exts, X509_EXTENSION_free);
  X509_REQ_free(req);

  if (!ok)
  {
    return fail(status_t::internal_server_error, "acme: failed to build CSR");
  }
  return der;
}

result_t<int64_t> cert_not_after_unix(std::string_view fullchain_pem)
{
  BIO *bio = BIO_new_mem_buf(fullchain_pem.data(), static_cast<int>(fullchain_pem.size()));
  if (bio == nullptr)
  {
    return fail(status_t::internal_server_error, "acme: BIO_new_mem_buf failed");
  }
  X509 *leaf = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  if (leaf == nullptr)
  {
    return fail(status_t::internal_server_error, "acme: no certificate in PEM");
  }
  const ASN1_TIME *not_after = X509_get0_notAfter(leaf);
  struct tm parsed = {};
  const bool ok = not_after != nullptr && ASN1_TIME_to_tm(not_after, &parsed) == 1;
  int64_t unix_time = 0;
  if (ok)
  {
    unix_time = tm_to_unix(parsed);
  }
  X509_free(leaf);
  if (!ok)
  {
    return fail(status_t::internal_server_error, "acme: failed to read notAfter");
  }
  return unix_time;
}
} // namespace prism::acme
