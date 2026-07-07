#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "../http.h"
#include "../status.h"

namespace prism::detail
{
struct codec_state_t;

struct parsed_request_t
{
  request_t request;
  bool keep_alive = false;
};

enum class feed_result_t : uint8_t
{
  ok,
  error,
};

class request_codec_t
{
public:
  request_codec_t();
  ~request_codec_t();

  request_codec_t(request_codec_t &&) noexcept;
  request_codec_t &operator=(request_codec_t &&) noexcept;
  request_codec_t(const request_codec_t &) = delete;
  request_codec_t &operator=(const request_codec_t &) = delete;

  feed_result_t feed(const char *data, std::size_t length);
  void finish();

  [[nodiscard]] bool has_request() const;
  parsed_request_t take_request();

  [[nodiscard]] status_t error_status() const;
  [[nodiscard]] bool current_in_progress() const;
  [[nodiscard]] bool current_headers_complete() const;

  // Configure size caps (0 disables a cap). Applied to subsequent parsing.
  void set_limits(std::size_t max_header_bytes, std::size_t max_body_bytes, std::size_t max_streaming_body_bytes);

  // Streaming-body surface. Valid once current_headers_complete() is true.
  [[nodiscard]] method_t current_method() const;
  [[nodiscard]] std::string_view current_path() const;
  [[nodiscard]] request_t take_header_request();
  [[nodiscard]] bool current_keep_alive() const;
  [[nodiscard]] std::optional<std::size_t> current_content_length() const;
  [[nodiscard]] bool current_is_chunked() const;

  // Switch the in-flight message to streaming body delivery: subsequent body
  // bytes accumulate in a pending buffer (drained by the reader) instead of
  // request_t::body, and completion does not enqueue into `ready`. Any body
  // bytes already parsed with the headers move into the pending buffer.
  void begin_streaming_body();
  // For a streaming route whose whole body already arrived: adopt it as the
  // pending buffer and mark the message complete.
  void preload_complete_body(std::string body);
  // Zero-copy path: while set, on_body does not append (bytes already landed in
  // the caller's buffer); llhttp still advances framing/completion.
  void set_suppress_body_append(bool suppress);

  [[nodiscard]] std::size_t pending_body_size() const;
  std::size_t take_body_into(std::span<std::byte> dst);
  std::string take_body_chunk();
  [[nodiscard]] bool message_complete() const;
  void discard_pending_body();

private:
  std::unique_ptr<codec_state_t> _impl;
};

std::string serialize_response(const response_t &response, bool keep_alive, bool head_request);

std::string serialize_streaming_headers(const response_t &response, bool keep_alive);
std::string serialize_chunk(std::string_view data);
std::string serialize_last_chunk();
} // namespace prism::detail
