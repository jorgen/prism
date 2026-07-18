#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace prism::detail::http2
{
inline constexpr std::string_view connection_preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

inline constexpr std::size_t frame_header_size = 9;
inline constexpr std::uint32_t default_max_frame_size = 16384;
inline constexpr std::uint32_t max_allowed_frame_size = 16777215;
inline constexpr std::size_t ping_payload_size = 8;

enum class frame_type_t : std::uint8_t
{
  data = 0x0,
  headers = 0x1,
  priority = 0x2,
  rst_stream = 0x3,
  settings = 0x4,
  push_promise = 0x5,
  ping = 0x6,
  goaway = 0x7,
  window_update = 0x8,
  continuation = 0x9,
};

namespace frame_flag
{
inline constexpr std::uint8_t end_stream = 0x1;
inline constexpr std::uint8_t ack = 0x1;
inline constexpr std::uint8_t end_headers = 0x4;
inline constexpr std::uint8_t padded = 0x8;
inline constexpr std::uint8_t priority = 0x20;
} // namespace frame_flag

enum class error_code_t : std::uint32_t
{
  no_error = 0x0,
  protocol_error = 0x1,
  internal_error = 0x2,
  flow_control_error = 0x3,
  settings_timeout = 0x4,
  stream_closed = 0x5,
  frame_size_error = 0x6,
  refused_stream = 0x7,
  cancel = 0x8,
  compression_error = 0x9,
  connect_error = 0xa,
  enhance_your_calm = 0xb,
  inadequate_security = 0xc,
  http_1_1_required = 0xd,
};

enum class settings_id_t : std::uint16_t
{
  header_table_size = 0x1,
  enable_push = 0x2,
  max_concurrent_streams = 0x3,
  initial_window_size = 0x4,
  max_frame_size = 0x5,
  max_header_list_size = 0x6,
  enable_connect_protocol = 0x8,
};

struct frame_header_t
{
  std::uint32_t length = 0;
  frame_type_t type = frame_type_t::data;
  std::uint8_t flags = 0;
  std::uint32_t stream_id = 0;

  [[nodiscard]] bool has_flag(std::uint8_t flag) const
  {
    return (flags & flag) != 0;
  }
};

struct frame_t
{
  frame_header_t header;
  std::string payload;
};

struct setting_t
{
  std::uint16_t id = 0;
  std::uint32_t value = 0;
};

struct goaway_t
{
  std::uint32_t last_stream_id = 0;
  error_code_t code = error_code_t::no_error;
  std::string debug;
};

enum class frame_status_t : std::uint8_t
{
  ok,
  error,
};

void write_frame_header(std::string &out, const frame_header_t &header);
std::string serialize_frame(frame_type_t type, std::uint8_t flags, std::uint32_t stream_id, std::string_view payload);

std::string serialize_settings(const std::vector<setting_t> &settings);
std::string serialize_settings_ack();
std::string serialize_ping(std::string_view opaque, bool ack);
std::string serialize_goaway(std::uint32_t last_stream_id, error_code_t code, std::string_view debug);
std::string serialize_rst_stream(std::uint32_t stream_id, error_code_t code);
std::string serialize_window_update(std::uint32_t stream_id, std::uint32_t increment);
std::string serialize_data(std::uint32_t stream_id, std::string_view data, bool end_stream);
std::string serialize_headers(std::uint32_t stream_id, std::string_view header_block, bool end_stream, bool end_headers);
std::string serialize_continuation(std::uint32_t stream_id, std::string_view header_block, bool end_headers);

bool parse_settings(std::string_view payload, std::vector<setting_t> &out);
bool parse_window_update(std::string_view payload, std::uint32_t &increment);
bool parse_rst_stream(std::string_view payload, error_code_t &code);
bool parse_goaway(std::string_view payload, goaway_t &out);

std::string_view data_without_padding(const frame_header_t &header, std::string_view payload, bool &ok);
std::string_view headers_block_fragment(const frame_header_t &header, std::string_view payload, bool &ok);

class frame_reader_t
{
public:
  frame_status_t feed(const char *data, std::size_t length);
  [[nodiscard]] bool has_frame() const;
  frame_t take_frame();
  [[nodiscard]] error_code_t error() const;
  void set_max_frame_size(std::uint32_t size);

private:
  std::string _buffer;
  std::deque<frame_t> _ready;
  std::uint32_t _max_frame_size = default_max_frame_size;
  error_code_t _error = error_code_t::no_error;
  bool _failed = false;
};
} // namespace prism::detail::http2
