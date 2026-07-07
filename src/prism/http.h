#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vio/task.h>

#include "status.h"

namespace vio
{
class event_loop_t;
}

namespace prism
{
enum class method_t : uint8_t
{
  get,
  head,
  post,
  put,
  patch,
  del, // "delete" is a keyword
  options,
  unknown,
};

constexpr std::string_view method_name(method_t m)
{
  switch (m)
  {
  case method_t::get:
    return "GET";
  case method_t::head:
    return "HEAD";
  case method_t::post:
    return "POST";
  case method_t::put:
    return "PUT";
  case method_t::patch:
    return "PATCH";
  case method_t::del:
    return "DELETE";
  case method_t::options:
    return "OPTIONS";
  case method_t::unknown:
    return "UNKNOWN";
  }
  return "UNKNOWN";
}

constexpr method_t method_from_name(std::string_view s)
{
  if (s == "GET")
    return method_t::get;
  if (s == "HEAD")
    return method_t::head;
  if (s == "POST")
    return method_t::post;
  if (s == "PUT")
    return method_t::put;
  if (s == "PATCH")
    return method_t::patch;
  if (s == "DELETE")
    return method_t::del;
  if (s == "OPTIONS")
    return method_t::options;
  return method_t::unknown;
}

// A header is a name/value pair. Kept as a flat vector to stay lean and to
// preserve ordering; lookups are linear, which is fine for the small header
// counts typical of REST traffic.
struct header_t
{
  std::string name;
  std::string value;
};

struct headers_t
{
  std::vector<header_t> entries;

  // Case-insensitive lookup. Returns nullptr if absent.
  [[nodiscard]] const std::string *find(std::string_view name) const;
  void set(std::string name, std::string value);
};

// A chunk of body bytes. On the response side `last` marks the final outbound
// chunk; on the request side a chunk with empty `data` and `last == true`
// signals end-of-body.
struct body_chunk_t
{
  std::string data;
  bool last = false;
};

// Terminal condition of a streaming request body. Errors are reported here
// rather than thrown (prism builds -fno-exceptions); the read methods return
// end-like values (empty+last / 0) when status is not ok/end.
enum class body_read_status_t : std::uint8_t
{
  ok,
  end,
  too_large,
  timed_out,
  aborted,
};

// A pull-based inbound body, handed to streaming-route handlers on request_t.
// The handler drives it: read_chunk() yields prism-owned chunks; read_into()
// fills the handler's own buffer (zero-copy on plain TCP); read_all() drains the
// rest. Backed by transport-specific state that outlives each suspension, so it
// is held by shared_ptr on request_t.
class request_body_t
{
public:
  virtual ~request_body_t() = default;

  request_body_t(const request_body_t &) = delete;
  request_body_t &operator=(const request_body_t &) = delete;

  [[nodiscard]] virtual vio::task_t<body_chunk_t> read_chunk() = 0;
  [[nodiscard]] virtual vio::task_t<std::size_t> read_into(std::span<std::byte> dst) = 0;
  [[nodiscard]] virtual vio::task_t<std::string> read_all() = 0;

  [[nodiscard]] virtual std::optional<std::size_t> length() const = 0;
  [[nodiscard]] virtual bool at_end() const = 0;
  [[nodiscard]] virtual body_read_status_t status() const = 0;

protected:
  request_body_t() = default;
};

struct request_t
{
  method_t method = method_t::get;
  std::string target; // path + optional query, e.g. "/users/42?verbose=1"
  std::string path;   // path component only, e.g. "/users/42"
  headers_t headers;
  std::string body; // fully buffered for non-streaming routes; empty when streaming

  // Path parameters captured by the router, e.g. {"id": "42"} for "/users/{id}".
  std::vector<header_t> params;

  vio::event_loop_t *loop = nullptr;

  // Set only for streaming routes (registered via app.*_stream). When present,
  // `body` is empty and the handler pulls the body through this reader.
  std::shared_ptr<request_body_t> body_reader;

  [[nodiscard]] std::string_view param(std::string_view name) const;
  [[nodiscard]] std::string query(std::string_view name) const;
  [[nodiscard]] bool has_query(std::string_view name) const;
  [[nodiscard]] std::span<const std::byte> raw_body() const;

  [[nodiscard]] bool is_streaming() const
  {
    return static_cast<bool>(body_reader);
  }
  [[nodiscard]] request_body_t &body_stream() const
  {
    return *body_reader;
  }
};

// A pull-based streaming response body: the server calls it repeatedly, each
// call returning the next chunk, until a chunk reports `last`. It may co_await
// async work (file/db/upstream reads). Capture producer state by value or
// shared_ptr — the source is stored in the response and outlives each pull.
using body_source_t = std::function<vio::task_t<body_chunk_t>()>;

struct response_t
{
  status_t status = status_t::ok;
  headers_t headers;
  std::string body;
  body_source_t body_stream;

  static response_t text(status_t status, std::string body);
  static response_t finished(status_t status, std::string content_type, std::string bytes);
  static response_t streaming(status_t status, std::string content_type, body_source_t source);
};
} // namespace prism
