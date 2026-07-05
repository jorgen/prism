#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include <prism/http.h>
#include <prism/static_files.h>

#include <vio/event_loop.h>
#include <vio/operation/file.h>
#include <vio/run.h>

namespace
{
prism::request_t static_request(vio::event_loop_t &loop, std::string tail)
{
  prism::request_t request;
  request.method = prism::method_t::get;
  request.path = "/" + tail;
  request.loop = &loop;
  request.params.push_back(prism::header_t{"path", std::move(tail)});
  return request;
}
} // namespace

TEST_CASE("content_type_for_path maps common extensions")
{
  CHECK(std::string(prism::content_type_for_path("index.html")) == "text/html; charset=utf-8");
  CHECK(std::string(prism::content_type_for_path("a/b/app.js")) == "text/javascript; charset=utf-8");
  CHECK(std::string(prism::content_type_for_path("style.CSS")) == "text/css; charset=utf-8");
  CHECK(std::string(prism::content_type_for_path("logo.png")) == "image/png");
  CHECK(std::string(prism::content_type_for_path("data.bin")) == "application/octet-stream");
  CHECK(std::string(prism::content_type_for_path("noext")) == "application/octet-stream");
  CHECK(std::string(prism::content_type_for_path("archive.tar.gz")) == "application/octet-stream");
}

TEST_CASE("static_file_handler serves a file and rejects traversal")
{
  int rc = vio::run(
    [](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      std::error_code ec;
      std::string tmpl = (std::filesystem::temp_directory_path(ec) / "prism_static_XXXXXX").string();
      REQUIRE(!ec);
      auto dir_or = vio::mkdtemp_path(loop, tmpl);
      REQUIRE(dir_or.has_value());
      std::string dir = dir_or.value();

      {
        std::ofstream file(dir + "/hello.txt", std::ios::binary);
        file << "hello static world";
      }

      prism::handler_t handler = prism::static_file_handler(dir);

      prism::response_t ok = co_await handler(static_request(loop, "hello.txt"));
      CHECK(ok.status == prism::status_t::ok);
      CHECK(ok.body == "hello static world");
      REQUIRE(ok.headers.find("Content-Type") != nullptr);
      CHECK(*ok.headers.find("Content-Type") == "text/plain; charset=utf-8");

      prism::response_t missing = co_await handler(static_request(loop, "nope.txt"));
      CHECK(missing.status == prism::status_t::not_found);

      prism::response_t traversal = co_await handler(static_request(loop, "../hello.txt"));
      CHECK(traversal.status == prism::status_t::not_found);

      co_return 0;
    });
  CHECK(rc == 0);
}
