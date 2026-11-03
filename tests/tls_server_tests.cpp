#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <prism/app.h>
#include <prism/detail/server.h>
#include <prism/router.h>
#include <prism/server_options.h>

#include <vio/cancellation.h>
#include <vio/operation/sleep.h>
#include <vio/operation/tcp.h>
#include <vio/operation/tcp_server.h>
#include <vio/operation/tls_client.h>
#include <vio/operation/tls_server.h>
#include <vio/run.h>
#include <vio/task.h>

namespace
{
struct test_certs_t
{
  std::vector<uint8_t> ca;
  std::vector<uint8_t> cert;
  std::vector<uint8_t> key;
};

std::vector<uint8_t> drain(BIO *bio)
{
  std::vector<uint8_t> data(BIO_pending(bio));
  BIO_read(bio, data.data(), static_cast<int>(data.size()));
  BIO_free(bio);
  return data;
}

EVP_PKEY *make_key()
{
  EVP_PKEY *pkey = nullptr;
  EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
  EVP_PKEY_keygen_init(ctx);
  EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx, NID_X9_62_prime256v1);
  EVP_PKEY_keygen(ctx, &pkey);
  EVP_PKEY_CTX_free(ctx);
  return pkey;
}

X509 *make_cert(EVP_PKEY *subject_key, const char *cn, X509_NAME *issuer, EVP_PKEY *issuer_key, long serial)
{
  X509 *x = X509_new();
  X509_set_version(x, 2);
  ASN1_INTEGER_set(X509_get_serialNumber(x), serial);
  X509_gmtime_adj(X509_get_notBefore(x), 0);
  X509_gmtime_adj(X509_get_notAfter(x), 86400L);
  X509_set_pubkey(x, subject_key);
  X509_NAME_add_entry_by_txt(X509_get_subject_name(x), "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char *>(cn), -1, -1, 0);
  X509_set_issuer_name(x, issuer != nullptr ? issuer : X509_get_subject_name(x));
  X509_sign(x, issuer_key, EVP_sha256());
  return x;
}

test_certs_t make_certs()
{
  EVP_PKEY *ca_key = make_key();
  X509 *ca = make_cert(ca_key, "prism test CA", nullptr, ca_key, 1);
  EVP_PKEY *leaf_key = make_key();
  X509 *leaf = make_cert(leaf_key, "localhost", X509_get_subject_name(ca), ca_key, 2);

  test_certs_t out;
  BIO *bio = BIO_new(BIO_s_mem());
  PEM_write_bio_X509(bio, ca);
  out.ca = drain(bio);
  bio = BIO_new(BIO_s_mem());
  PEM_write_bio_X509(bio, leaf);
  out.cert = drain(bio);
  bio = BIO_new(BIO_s_mem());
  PEM_write_bio_PrivateKey(bio, leaf_key, nullptr, nullptr, 0, nullptr, nullptr);
  out.key = drain(bio);

  X509_free(ca);
  X509_free(leaf);
  EVP_PKEY_free(ca_key);
  EVP_PKEY_free(leaf_key);
  return out;
}

struct tls_server_t
{
  int port = 0;
  vio::cancellation_t cancel;
  std::optional<vio::task_t<prism::result_t<void>>> task;
};

void start_tls_server(vio::event_loop_t &loop, const test_certs_t &certs, prism::server_options_t options, tls_server_t &out)
{
  prism::app_t app;
  app.get("/ping", [](prism::request_t) -> vio::task_t<prism::response_t> { co_return prism::response_t::text(prism::status_t::ok, "pong"); });

  auto addr = vio::ip4_addr("127.0.0.1", 0);
  REQUIRE(addr.has_value());
  auto server = vio::tcp_create_server(loop);
  REQUIRE(server.has_value());
  REQUIRE(vio::tcp_bind(server.value(), reinterpret_cast<const sockaddr *>(&addr.value())).has_value());
  auto name = vio::sockname(server->tcp);
  REQUIRE(name.has_value());
  out.port = ntohs(reinterpret_cast<const sockaddr_in *>(&name.value())->sin_port);

  vio::ssl_config_t config{.cert_mem = certs.cert, .key_mem = certs.key};
  auto tls = vio::ssl_server_create(loop, std::move(server.value()), "localhost", config);
  REQUIRE(tls.has_value());
  out.task.emplace(prism::detail::serve_tls(std::move(tls.value()), std::make_shared<const prism::router_t>(app.router()), nullptr, &out.cancel, options));
}

// Connects, sends nothing, and reports whether the server closed the connection
// (as opposed to the 5 s guard giving up).
vio::task_t<bool> silent_client_closed(vio::event_loop_t &loop, int port)
{
  auto tcp = vio::tcp_create(loop);
  REQUIRE(tcp.has_value());
  auto addr = vio::ip4_addr("127.0.0.1", port);
  REQUIRE(addr.has_value());
  auto connected = co_await vio::tcp_connect(tcp.value(), reinterpret_cast<const sockaddr *>(&addr.value()));
  REQUIRE(connected.has_value());
  auto reader = vio::tcp_create_reader(tcp.value());
  REQUIRE(reader.has_value());

  vio::cancellation_t stop;
  auto guard = [](vio::event_loop_t &el, vio::tcp_reader_t &rd, vio::cancellation_t &stp) -> vio::task_t<void>
  {
    auto slept = co_await vio::sleep(el, std::chrono::seconds(5), &stp);
    if (slept.has_value())
    {
      rd.cancel();
    }
  }(loop, reader.value(), stop);

  bool closed = false;
  for (;;)
  {
    auto chunk = co_await reader.value();
    if (!chunk.has_value())
    {
      closed = !vio::is_cancelled(chunk.error());
      break;
    }
  }
  stop.cancel();
  co_await std::move(guard);
  co_return bool{closed};
}
} // namespace

TEST_CASE("tls: a client that never completes the handshake is closed after handshake_timeout")
{
  auto certs = make_certs();
  int rc = vio::run(
    [&certs](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      prism::server_options_t options;
      options.handshake_timeout = std::chrono::milliseconds{100};
      options.worker_threads = 1;
      tls_server_t server;
      start_tls_server(loop, certs, options, server);

      auto start = std::chrono::steady_clock::now();
      bool closed = co_await silent_client_closed(loop, server.port);
      auto elapsed = std::chrono::steady_clock::now() - start;
      CHECK(closed);
      CHECK(elapsed < std::chrono::seconds{2});

      server.cancel.cancel();
      auto result = co_await std::move(*server.task);
      CHECK(result.has_value());
      co_return 0;
    });
  CHECK(rc == 0);
}

TEST_CASE("tls: a normal client is served with handshake_timeout set")
{
  auto certs = make_certs();
  int rc = vio::run(
    [&certs](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      prism::server_options_t options;
      options.handshake_timeout = std::chrono::seconds{5};
      tls_server_t server;
      start_tls_server(loop, certs, options, server);

      std::string response;
      {
        vio::ssl_config_t client_config{.ca_mem = certs.ca};
        auto client = vio::ssl_client_create(loop, client_config);
        REQUIRE(client.has_value());
        auto c = std::move(client.value());
        auto connected = co_await vio::ssl_client_connect(c, "localhost", server.port, "127.0.0.1");
        REQUIRE(connected.has_value());
        std::string request = "GET /ping HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        auto written = co_await vio::ssl_client_write(c, std::as_bytes(std::span(request)));
        REQUIRE(written.has_value());
        auto reader = vio::ssl_client_create_reader(c);
        REQUIRE(reader.has_value());
        for (;;)
        {
          auto chunk = co_await reader.value();
          if (!chunk.has_value())
          {
            break;
          }
          response.append(reinterpret_cast<const char *>(chunk.value().data()), chunk.value().size());
        }
      }
      CHECK(response.find("HTTP/1.1 200 OK") != std::string::npos);
      CHECK(response.ends_with("pong"));

      server.cancel.cancel();
      auto result = co_await std::move(*server.task);
      CHECK(result.has_value());
      co_return 0;
    });
  CHECK(rc == 0);
}
