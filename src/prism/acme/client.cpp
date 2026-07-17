#include "client.h"

#include <utility>

#include <structify/structify.h>

#include <vio/crypto.h>
#include <vio/operation/sleep.h>

#include "../json.h"
#include "csr.h"

namespace prism::acme
{
namespace
{
struct directory_json_t
{
  std::string newNonce;
  std::string newAccount;
  std::string newOrder;
  STFY_OBJ(newNonce, newAccount, newOrder);
};

struct identifier_json_t
{
  std::string type;
  std::string value;
  STFY_OBJ(type, value);
};

struct challenge_json_t
{
  std::string type;
  std::string url;
  std::string token;
  std::string status;
  STFY_OBJ(type, url, token, status);
};

struct authorization_json_t
{
  std::string status;
  identifier_json_t identifier;
  std::vector<challenge_json_t> challenges;
  STFY_OBJ(status, identifier, challenges);
};

struct order_json_t
{
  std::string status;
  std::vector<std::string> authorizations;
  std::string finalize;
  std::optional<std::string> certificate;
  STFY_OBJ(status, authorizations, finalize, certificate);
};

struct problem_json_t
{
  std::string type;
  std::string detail;
  STFY_OBJ(type, detail);
};

std::string identifiers_payload(const std::vector<std::string> &domains)
{
  std::string out = "{\"identifiers\":[";
  for (size_t i = 0; i < domains.size(); ++i)
  {
    if (i != 0)
    {
      out += ",";
    }
    out += "{\"type\":\"dns\",\"value\":\"" + domains[i] + "\"}";
  }
  out += "]}";
  return out;
}
} // namespace

client_t::client_t(vio::event_loop_t &loop, client_config_t config, ec_key_t account_key)
  : _loop(loop)
  , _config(std::move(config))
  , _account_key(std::move(account_key))
{
}

void client_t::note(std::string_view message) const
{
  if (_config.log)
  {
    _config.log(message);
  }
}

vio::task_t<result_t<void>> client_t::ensure_directory(vio::cancellation_t *cancel)
{
  if (_have_directory)
  {
    co_return result_t<void>{};
  }
  vio::http::request_t request;
  request.method = "GET";
  request.url = _config.directory_url;
  if (_config.ca_mem)
  {
    request.ca_mem = _config.ca_mem;
  }
  auto response = co_await vio::http::fetch_once(_loop, request, cancel);
  if (!response)
  {
    co_return fail(status_t::bad_gateway, "acme: directory fetch failed: " + response.error().msg);
  }
  if (response->status != 200)
  {
    co_return fail(status_t::bad_gateway, "acme: directory returned status " + std::to_string(response->status));
  }
  auto directory = prism::json::parse<directory_json_t>(response->body);
  if (!directory)
  {
    co_return std::unexpected(directory.error());
  }
  _new_nonce_url = directory->newNonce;
  _new_account_url = directory->newAccount;
  _new_order_url = directory->newOrder;
  _have_directory = true;
  co_return result_t<void>{};
}

vio::task_t<result_t<void>> client_t::refresh_nonce(vio::cancellation_t *cancel)
{
  if (auto directory = co_await ensure_directory(cancel); !directory)
  {
    co_return std::unexpected(directory.error());
  }
  vio::http::request_t request;
  request.method = "HEAD";
  request.url = _new_nonce_url;
  if (_config.ca_mem)
  {
    request.ca_mem = _config.ca_mem;
  }
  auto response = co_await vio::http::fetch_once(_loop, request, cancel);
  if (!response)
  {
    co_return fail(status_t::bad_gateway, "acme: newNonce failed: " + response.error().msg);
  }
  auto nonce = response->header("Replay-Nonce");
  if (nonce.empty())
  {
    co_return fail(status_t::bad_gateway, "acme: newNonce returned no Replay-Nonce");
  }
  _nonce.assign(nonce);
  co_return result_t<void>{};
}

vio::task_t<result_t<vio::http::response_t>> client_t::post_jws(const std::string &url, const std::string &payload, bool use_jwk, vio::cancellation_t *cancel)
{
  for (int attempt = 0; attempt < 2; ++attempt)
  {
    if (_nonce.empty())
    {
      if (auto refreshed = co_await refresh_nonce(cancel); !refreshed)
      {
        co_return std::unexpected(refreshed.error());
      }
    }

    std::string protected_json;
    if (use_jwk)
    {
      auto jwk = jwk_json(_account_key);
      if (!jwk)
      {
        co_return std::unexpected(jwk.error());
      }
      protected_json = "{\"alg\":\"ES256\",\"jwk\":" + *jwk + ",\"nonce\":\"" + _nonce + "\",\"url\":\"" + url + "\"}";
    }
    else
    {
      protected_json = "{\"alg\":\"ES256\",\"kid\":\"" + _kid + "\",\"nonce\":\"" + _nonce + "\",\"url\":\"" + url + "\"}";
    }

    auto jws = es256_jws(_account_key, protected_json, payload);
    if (!jws)
    {
      co_return std::unexpected(jws.error());
    }

    vio::http::request_t request;
    request.method = "POST";
    request.url = url;
    request.body = *jws;
    request.headers.push_back(vio::http::header_t{"Content-Type", "application/jose+json"});
    if (_config.ca_mem)
    {
      request.ca_mem = _config.ca_mem;
    }
    auto response = co_await vio::http::fetch_once(_loop, request, cancel);
    if (!response)
    {
      co_return fail(status_t::bad_gateway, "acme: request to " + url + " failed: " + response.error().msg);
    }

    auto nonce = response->header("Replay-Nonce");
    if (!nonce.empty())
    {
      _nonce.assign(nonce);
    }
    else
    {
      _nonce.clear();
    }

    if (response->status >= 400)
    {
      std::string type;
      std::string detail;
      if (auto problem = prism::json::parse<problem_json_t>(response->body))
      {
        type = problem->type;
        detail = problem->detail;
      }
      if (attempt == 0 && type.find("badNonce") != std::string::npos)
      {
        note("acme: badNonce, retrying with a fresh nonce");
        continue;
      }
      co_return fail(status_t::bad_gateway, "acme: " + url + " -> " + std::to_string(response->status) + " " + type + " " + detail);
    }

    co_return std::move(*response);
  }
  co_return fail(status_t::bad_gateway, "acme: exhausted nonce retries for " + url);
}

vio::task_t<result_t<vio::http::response_t>> client_t::post_as_get(const std::string &url, vio::cancellation_t *cancel)
{
  co_return co_await post_jws(url, std::string{}, false, cancel);
}

vio::task_t<result_t<void>> client_t::ensure_account(vio::cancellation_t *cancel)
{
  if (!_kid.empty())
  {
    co_return result_t<void>{};
  }
  if (auto directory = co_await ensure_directory(cancel); !directory)
  {
    co_return std::unexpected(directory.error());
  }

  std::string payload = "{\"termsOfServiceAgreed\":true";
  if (!_config.contact_email.empty())
  {
    payload += ",\"contact\":[\"mailto:" + _config.contact_email + "\"]";
  }
  payload += "}";

  auto response = co_await post_jws(_new_account_url, payload, true, cancel);
  if (!response)
  {
    co_return std::unexpected(response.error());
  }
  auto location = response->header("Location");
  if (location.empty())
  {
    co_return fail(status_t::bad_gateway, "acme: newAccount returned no Location");
  }
  _kid.assign(location);
  note("acme: account ready");
  co_return result_t<void>{};
}

vio::task_t<result_t<issued_cert_t>> client_t::obtain_certificate(std::vector<std::string> domains, challenge_publish_t publish, challenge_cleanup_t cleanup, vio::cancellation_t *cancel)
{
  if (domains.empty())
  {
    co_return fail(status_t::internal_server_error, "acme: no domains for order");
  }
  if (auto account = co_await ensure_account(cancel); !account)
  {
    co_return std::unexpected(account.error());
  }

  auto order_response = co_await post_jws(_new_order_url, identifiers_payload(domains), false, cancel);
  if (!order_response)
  {
    co_return std::unexpected(order_response.error());
  }
  const std::string order_url(order_response->header("Location"));
  auto order = prism::json::parse<order_json_t>(order_response->body);
  if (!order)
  {
    co_return std::unexpected(order.error());
  }
  if (order_url.empty())
  {
    co_return fail(status_t::bad_gateway, "acme: newOrder returned no Location");
  }

  auto thumbprint = jwk_thumbprint(_account_key);
  if (!thumbprint)
  {
    co_return std::unexpected(thumbprint.error());
  }

  for (const auto &authorization_url : order->authorizations)
  {
    auto authorization_response = co_await post_as_get(authorization_url, cancel);
    if (!authorization_response)
    {
      co_return std::unexpected(authorization_response.error());
    }
    auto authorization = prism::json::parse<authorization_json_t>(authorization_response->body);
    if (!authorization)
    {
      co_return std::unexpected(authorization.error());
    }
    if (authorization->status == "valid")
    {
      continue;
    }

    const challenge_json_t *http01 = nullptr;
    for (const auto &challenge : authorization->challenges)
    {
      if (challenge.type == "http-01")
      {
        http01 = &challenge;
        break;
      }
    }
    if (http01 == nullptr)
    {
      co_return fail(status_t::bad_gateway, "acme: no http-01 challenge for " + authorization->identifier.value);
    }

    const std::string token = http01->token;
    const std::string challenge_url = http01->url;
    publish(token, http01_key_authorization(token, *thumbprint));

    auto answer = co_await post_jws(challenge_url, "{}", false, cancel);
    if (!answer)
    {
      cleanup(token);
      co_return std::unexpected(answer.error());
    }

    result_t<void> outcome = fail(status_t::bad_gateway, "acme: authorization for " + authorization->identifier.value + " did not validate");
    for (int attempt = 0; attempt < _config.max_poll_attempts; ++attempt)
    {
      auto polled = co_await post_as_get(authorization_url, cancel);
      if (!polled)
      {
        outcome = std::unexpected(polled.error());
        break;
      }
      auto state = prism::json::parse<authorization_json_t>(polled->body);
      if (!state)
      {
        outcome = std::unexpected(state.error());
        break;
      }
      if (state->status == "valid")
      {
        outcome = result_t<void>{};
        break;
      }
      if (state->status == "invalid")
      {
        outcome = fail(status_t::bad_gateway, "acme: authorization for " + authorization->identifier.value + " is invalid");
        break;
      }
      co_await vio::sleep(_loop, _config.poll_interval, cancel);
    }

    cleanup(token);
    if (!outcome)
    {
      co_return std::unexpected(outcome.error());
    }
  }

  auto cert_key = ec_key_t::generate();
  if (!cert_key)
  {
    co_return std::unexpected(cert_key.error());
  }
  auto csr = make_csr_der(*cert_key, domains);
  if (!csr)
  {
    co_return std::unexpected(csr.error());
  }
  const std::string finalize_payload = "{\"csr\":\"" + vio::crypto::base64url_encode(*csr) + "\"}";
  auto finalize_response = co_await post_jws(order->finalize, finalize_payload, false, cancel);
  if (!finalize_response)
  {
    co_return std::unexpected(finalize_response.error());
  }

  std::string certificate_url;
  for (int attempt = 0; attempt < _config.max_poll_attempts; ++attempt)
  {
    auto polled = co_await post_as_get(order_url, cancel);
    if (!polled)
    {
      co_return std::unexpected(polled.error());
    }
    auto state = prism::json::parse<order_json_t>(polled->body);
    if (!state)
    {
      co_return std::unexpected(state.error());
    }
    if (state->status == "valid")
    {
      if (state->certificate)
      {
        certificate_url = *state->certificate;
      }
      break;
    }
    if (state->status == "invalid")
    {
      co_return fail(status_t::bad_gateway, "acme: order became invalid");
    }
    co_await vio::sleep(_loop, _config.poll_interval, cancel);
  }
  if (certificate_url.empty())
  {
    co_return fail(status_t::bad_gateway, "acme: order did not yield a certificate");
  }

  auto certificate_response = co_await post_as_get(certificate_url, cancel);
  if (!certificate_response)
  {
    co_return std::unexpected(certificate_response.error());
  }
  if (certificate_response->status != 200)
  {
    co_return fail(status_t::bad_gateway, "acme: certificate download returned status " + std::to_string(certificate_response->status));
  }

  auto cert_key_pem = cert_key->to_pem();
  if (!cert_key_pem)
  {
    co_return std::unexpected(cert_key_pem.error());
  }
  note("acme: certificate issued");
  co_return issued_cert_t{.fullchain_pem = certificate_response->body, .cert_key_pem = *cert_key_pem};
}
} // namespace prism::acme
