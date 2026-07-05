#include "frame.h"

#include <utility>

namespace prism::detail::http2
{
namespace
{
void put_u16(std::string &out, std::uint16_t v)
{
  out.push_back(static_cast<char>((v >> 8) & 0xff));
  out.push_back(static_cast<char>(v & 0xff));
}

void put_u24(std::string &out, std::uint32_t v)
{
  out.push_back(static_cast<char>((v >> 16) & 0xff));
  out.push_back(static_cast<char>((v >> 8) & 0xff));
  out.push_back(static_cast<char>(v & 0xff));
}

void put_u32(std::string &out, std::uint32_t v)
{
  out.push_back(static_cast<char>((v >> 24) & 0xff));
  out.push_back(static_cast<char>((v >> 16) & 0xff));
  out.push_back(static_cast<char>((v >> 8) & 0xff));
  out.push_back(static_cast<char>(v & 0xff));
}

std::uint16_t get_u16(const unsigned char *p)
{
  return static_cast<std::uint16_t>((static_cast<std::uint32_t>(p[0]) << 8) | static_cast<std::uint32_t>(p[1]));
}

std::uint32_t get_u24(const unsigned char *p)
{
  return (static_cast<std::uint32_t>(p[0]) << 16) | (static_cast<std::uint32_t>(p[1]) << 8) | static_cast<std::uint32_t>(p[2]);
}

std::uint32_t get_u32(const unsigned char *p)
{
  return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) | (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

const unsigned char *as_bytes(std::string_view s)
{
  return reinterpret_cast<const unsigned char *>(s.data());
}
} // namespace

void write_frame_header(std::string &out, const frame_header_t &header)
{
  put_u24(out, header.length);
  out.push_back(static_cast<char>(static_cast<std::uint8_t>(header.type)));
  out.push_back(static_cast<char>(header.flags));
  put_u32(out, header.stream_id & 0x7fffffff);
}

std::string serialize_frame(frame_type_t type, std::uint8_t flags, std::uint32_t stream_id, std::string_view payload)
{
  std::string out;
  out.reserve(frame_header_size + payload.size());
  frame_header_t header{static_cast<std::uint32_t>(payload.size()), type, flags, stream_id};
  write_frame_header(out, header);
  out.append(payload);
  return out;
}

std::string serialize_settings(const std::vector<setting_t> &settings)
{
  std::string payload;
  payload.reserve(settings.size() * 6);
  for (const setting_t &s : settings)
  {
    put_u16(payload, s.id);
    put_u32(payload, s.value);
  }
  return serialize_frame(frame_type_t::settings, 0, 0, payload);
}

std::string serialize_settings_ack()
{
  return serialize_frame(frame_type_t::settings, frame_flag::ack, 0, {});
}

std::string serialize_ping(std::string_view opaque, bool ack)
{
  std::string payload(opaque.substr(0, ping_payload_size));
  payload.resize(ping_payload_size, '\0');
  return serialize_frame(frame_type_t::ping, ack ? frame_flag::ack : std::uint8_t{0}, 0, payload);
}

std::string serialize_goaway(std::uint32_t last_stream_id, error_code_t code, std::string_view debug)
{
  std::string payload;
  put_u32(payload, last_stream_id & 0x7fffffff);
  put_u32(payload, static_cast<std::uint32_t>(code));
  payload.append(debug);
  return serialize_frame(frame_type_t::goaway, 0, 0, payload);
}

std::string serialize_rst_stream(std::uint32_t stream_id, error_code_t code)
{
  std::string payload;
  put_u32(payload, static_cast<std::uint32_t>(code));
  return serialize_frame(frame_type_t::rst_stream, 0, stream_id, payload);
}

std::string serialize_window_update(std::uint32_t stream_id, std::uint32_t increment)
{
  std::string payload;
  put_u32(payload, increment & 0x7fffffff);
  return serialize_frame(frame_type_t::window_update, 0, stream_id, payload);
}

std::string serialize_data(std::uint32_t stream_id, std::string_view data, bool end_stream)
{
  return serialize_frame(frame_type_t::data, end_stream ? frame_flag::end_stream : std::uint8_t{0}, stream_id, data);
}

std::string serialize_headers(std::uint32_t stream_id, std::string_view header_block, bool end_stream, bool end_headers)
{
  std::uint8_t flags = 0;
  if (end_stream)
  {
    flags = static_cast<std::uint8_t>(flags | frame_flag::end_stream);
  }
  if (end_headers)
  {
    flags = static_cast<std::uint8_t>(flags | frame_flag::end_headers);
  }
  return serialize_frame(frame_type_t::headers, flags, stream_id, header_block);
}

std::string serialize_continuation(std::uint32_t stream_id, std::string_view header_block, bool end_headers)
{
  return serialize_frame(frame_type_t::continuation, end_headers ? frame_flag::end_headers : std::uint8_t{0}, stream_id, header_block);
}

bool parse_settings(std::string_view payload, std::vector<setting_t> &out)
{
  if (payload.size() % 6 != 0)
  {
    return false;
  }
  const unsigned char *p = as_bytes(payload);
  for (std::size_t i = 0; i < payload.size(); i += 6)
  {
    out.push_back(setting_t{get_u16(p + i), get_u32(p + i + 2)});
  }
  return true;
}

bool parse_window_update(std::string_view payload, std::uint32_t &increment)
{
  if (payload.size() != 4)
  {
    return false;
  }
  increment = get_u32(as_bytes(payload)) & 0x7fffffff;
  return true;
}

bool parse_rst_stream(std::string_view payload, error_code_t &code)
{
  if (payload.size() != 4)
  {
    return false;
  }
  code = static_cast<error_code_t>(get_u32(as_bytes(payload)));
  return true;
}

bool parse_goaway(std::string_view payload, goaway_t &out)
{
  if (payload.size() < 8)
  {
    return false;
  }
  const unsigned char *p = as_bytes(payload);
  out.last_stream_id = get_u32(p) & 0x7fffffff;
  out.code = static_cast<error_code_t>(get_u32(p + 4));
  out.debug.assign(payload.substr(8));
  return true;
}

std::string_view data_without_padding(const frame_header_t &header, std::string_view payload, bool &ok)
{
  ok = true;
  if (!header.has_flag(frame_flag::padded))
  {
    return payload;
  }
  if (payload.empty())
  {
    ok = false;
    return {};
  }
  std::size_t pad = static_cast<unsigned char>(payload[0]);
  payload.remove_prefix(1);
  if (pad > payload.size())
  {
    ok = false;
    return {};
  }
  payload.remove_suffix(pad);
  return payload;
}

std::string_view headers_block_fragment(const frame_header_t &header, std::string_view payload, bool &ok)
{
  ok = true;
  std::size_t pad = 0;
  if (header.has_flag(frame_flag::padded))
  {
    if (payload.empty())
    {
      ok = false;
      return {};
    }
    pad = static_cast<unsigned char>(payload[0]);
    payload.remove_prefix(1);
  }
  if (header.has_flag(frame_flag::priority))
  {
    if (payload.size() < 5)
    {
      ok = false;
      return {};
    }
    payload.remove_prefix(5);
  }
  if (pad > payload.size())
  {
    ok = false;
    return {};
  }
  payload.remove_suffix(pad);
  return payload;
}

frame_status_t frame_reader_t::feed(const char *data, std::size_t length)
{
  if (_failed)
  {
    return frame_status_t::error;
  }
  _buffer.append(data, length);
  std::size_t pos = 0;
  for (;;)
  {
    if (_buffer.size() - pos < frame_header_size)
    {
      break;
    }
    const unsigned char *p = as_bytes(_buffer) + pos;
    std::uint32_t len = get_u24(p);
    if (len > _max_frame_size)
    {
      _error = error_code_t::frame_size_error;
      _failed = true;
      _buffer.clear();
      return frame_status_t::error;
    }
    if (_buffer.size() - pos < frame_header_size + len)
    {
      break;
    }
    frame_t frame;
    frame.header.length = len;
    frame.header.type = static_cast<frame_type_t>(p[3]);
    frame.header.flags = p[4];
    frame.header.stream_id = get_u32(p + 5) & 0x7fffffff;
    frame.payload.assign(_buffer, pos + frame_header_size, len);
    _ready.push_back(std::move(frame));
    pos += frame_header_size + len;
  }
  if (pos > 0)
  {
    _buffer.erase(0, pos);
  }
  return frame_status_t::ok;
}

bool frame_reader_t::has_frame() const
{
  return !_ready.empty();
}

frame_t frame_reader_t::take_frame()
{
  frame_t frame = std::move(_ready.front());
  _ready.pop_front();
  return frame;
}

error_code_t frame_reader_t::error() const
{
  return _error;
}

void frame_reader_t::set_max_frame_size(std::uint32_t size)
{
  _max_frame_size = size;
}
} // namespace prism::detail::http2
