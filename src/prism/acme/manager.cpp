#include "manager.h"

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <span>
#include <utility>

#include <vio/crypto.h>
#include <vio/operation/sleep.h>
#include <vio/ssl_context.h>

#include "../app.h"
#include "../http.h"
#include "csr.h"

namespace prism::acme
{
namespace
{
namespace fs = std::filesystem;

std::span<const uint8_t> as_bytes(const std::string &value)
{
  return {reinterpret_cast<const uint8_t *>(value.data()), value.size()};
}

std::optional<std::string> read_file(const std::string &path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open())
  {
    return std::nullopt;
  }
  return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

bool write_file(const std::string &path, const std::string &content, bool secret)
{
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output.is_open())
  {
    return false;
  }
  output.write(content.data(), static_cast<std::streamsize>(content.size()));
  output.flush();
  if (!output.good())
  {
    return false;
  }
  output.close();
  std::error_code ec;
  const fs::perms mode = secret ? (fs::perms::owner_read | fs::perms::owner_write) : (fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::others_read);
  fs::permissions(path, mode, fs::perm_options::replace, ec);
  return true;
}

std::string directory_hash(const std::string &url)
{
  auto digest = vio::crypto::sha256(as_bytes(url));
  return vio::crypto::to_hex(digest).substr(0, 16);
}

vio::task_t<response_t> serve_challenge(std::shared_ptr<challenge_store_t> store, request_t request)
{
  auto value = store->get(std::string(request.param("token")));
  if (!value)
  {
    co_return response_t::text(status_t::not_found, "");
  }
  response_t response;
  response.status = status_t::ok;
  response.body = std::move(*value);
  response.headers.set("Content-Type", "application/octet-stream");
  co_return response;
}
} // namespace

void challenge_store_t::put(const std::string &token, std::string key_authorization)
{
  std::lock_guard<std::mutex> lock(_mutex);
  _entries[token] = std::move(key_authorization);
}

void challenge_store_t::remove(const std::string &token)
{
  std::lock_guard<std::mutex> lock(_mutex);
  _entries.erase(token);
}

std::optional<std::string> challenge_store_t::get(const std::string &token) const
{
  std::lock_guard<std::mutex> lock(_mutex);
  auto it = _entries.find(token);
  if (it == _entries.end())
  {
    return std::nullopt;
  }
  return it->second;
}

manager_t::manager_t(vio::event_loop_t &loop, manager_config_t config)
  : _loop(loop)
  , _config(std::move(config))
  , _challenge_store(std::make_shared<challenge_store_t>())
{
}

void manager_t::note(std::string_view message) const
{
  if (_config.log)
  {
    _config.log(message);
  }
}

std::string manager_t::account_dir() const
{
  return _config.storage_dir + "/" + directory_hash(_config.directory_url);
}

std::string manager_t::domain_dir(const std::string &domain) const
{
  return account_dir() + "/" + domain;
}

void manager_t::install_challenge_route(app_t &http_app)
{
  http_app.get("/.well-known/acme-challenge/{token}", std::bind_front(&serve_challenge, _challenge_store));
}

result_t<void> manager_t::ensure_client()
{
  if (_client)
  {
    return result_t<void>{};
  }
  std::error_code ec;
  fs::create_directories(account_dir(), ec);

  const std::string key_path = account_dir() + "/account.key";
  ec_key_t account_key;
  if (auto pem = read_file(key_path))
  {
    auto key = ec_key_t::from_pem(*pem);
    if (!key)
    {
      return std::unexpected(key.error());
    }
    account_key = std::move(*key);
  }
  else
  {
    auto key = ec_key_t::generate();
    if (!key)
    {
      return std::unexpected(key.error());
    }
    auto key_pem = key->to_pem();
    if (!key_pem)
    {
      return std::unexpected(key_pem.error());
    }
    if (!write_file(key_path, *key_pem, true))
    {
      return fail(status_t::internal_server_error, "acme: cannot persist account key at " + key_path);
    }
    account_key = std::move(*key);
  }

  client_config_t client_config;
  client_config.directory_url = _config.directory_url;
  client_config.contact_email = _config.contact_email;
  client_config.ca_mem = _config.ca_mem;
  client_config.log = _config.log;
  _client = std::make_unique<client_t>(_loop, std::move(client_config), std::move(account_key));
  return result_t<void>{};
}

vio::task_t<result_t<void>> manager_t::process_domain(const std::string &domain, std::shared_ptr<vio::sni_cert_store_t> sni_store, bool install_valid, vio::cancellation_t *cancel)
{
  const std::string dir = domain_dir(domain);
  const std::string cert_path = dir + "/cert.pem";
  const std::string key_path = dir + "/key.pem";

  auto stored_cert = read_file(cert_path);
  auto stored_key = read_file(key_path);
  bool needs_issue = true;
  if (stored_cert && stored_key)
  {
    if (auto not_after = cert_not_after_unix(*stored_cert))
    {
      const int64_t now = static_cast<int64_t>(std::time(nullptr));
      if (*not_after - now > static_cast<int64_t>(_config.renew_before.count()))
      {
        needs_issue = false;
      }
    }
  }

  if (!needs_issue)
  {
    if (install_valid)
    {
      if (auto installed = sni_store->set_certificate(domain, as_bytes(*stored_cert), as_bytes(*stored_key)); !installed)
      {
        co_return fail(status_t::internal_server_error, "acme: installing stored certificate failed: " + installed.error().msg);
      }
    }
    co_return result_t<void>{};
  }

  if (auto ready = ensure_client(); !ready)
  {
    co_return std::unexpected(ready.error());
  }

  auto store = _challenge_store;
  challenge_publish_t publish = [store](const std::string &token, const std::string &key_authorization) { store->put(token, key_authorization); };
  challenge_cleanup_t cleanup = [store](const std::string &token) { store->remove(token); };

  note(std::string("acme: requesting certificate for ") + domain);
  auto issued = co_await _client->obtain_certificate({domain}, publish, cleanup, cancel);
  if (!issued)
  {
    co_return std::unexpected(issued.error());
  }

  std::error_code ec;
  fs::create_directories(dir, ec);
  if (!write_file(cert_path, issued->fullchain_pem, false))
  {
    co_return fail(status_t::internal_server_error, "acme: cannot persist certificate at " + cert_path);
  }
  if (!write_file(key_path, issued->cert_key_pem, true))
  {
    co_return fail(status_t::internal_server_error, "acme: cannot persist certificate key at " + key_path);
  }

  if (auto installed = sni_store->set_certificate(domain, as_bytes(issued->fullchain_pem), as_bytes(issued->cert_key_pem)); !installed)
  {
    co_return fail(status_t::internal_server_error, "acme: installing issued certificate failed: " + installed.error().msg);
  }
  note(std::string("acme: certificate ready for ") + domain);
  co_return result_t<void>{};
}

vio::task_t<result_t<void>> manager_t::ensure_certificates(std::shared_ptr<vio::sni_cert_store_t> sni_store, vio::cancellation_t *cancel)
{
  if (auto ready = ensure_client(); !ready)
  {
    co_return std::unexpected(ready.error());
  }
  for (const auto &domain : _config.domains)
  {
    auto processed = co_await process_domain(domain, sni_store, true, cancel);
    if (!processed)
    {
      note(std::string("acme: certificate provisioning failed for ") + domain + ": " + processed.error().msg);
    }
  }
  co_return result_t<void>{};
}

vio::task_t<void> manager_t::run_renewal_loop(std::shared_ptr<vio::sni_cert_store_t> sni_store, vio::cancellation_t *cancel)
{
  const auto interval = std::chrono::duration_cast<std::chrono::milliseconds>(_config.renewal_interval);
  const auto backoff = std::chrono::duration_cast<std::chrono::milliseconds>(_config.renewal_backoff);
  while (cancel == nullptr || !cancel->is_cancelled())
  {
    co_await vio::sleep(_loop, interval, cancel);
    if (cancel != nullptr && cancel->is_cancelled())
    {
      break;
    }
    for (const auto &domain : _config.domains)
    {
      auto processed = co_await process_domain(domain, sni_store, false, cancel);
      if (!processed)
      {
        note(std::string("acme: renewal failed for ") + domain + ": " + processed.error().msg);
        co_await vio::sleep(_loop, backoff, cancel);
      }
    }
  }
}
} // namespace prism::acme
