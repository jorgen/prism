#pragma once

#include <cstddef>
#include <memory>
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

private:
  std::unique_ptr<codec_state_t> _impl;
};

std::string serialize_response(const response_t &response, bool keep_alive, bool head_request);

std::string serialize_streaming_headers(const response_t &response, bool keep_alive);
std::string serialize_chunk(std::string_view data);
std::string serialize_last_chunk();
} // namespace prism::detail
