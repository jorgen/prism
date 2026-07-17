#include "frame.h"

namespace prism::detail::websocket
{
bool is_control(opcode_t opcode)
{
  return (static_cast<std::uint8_t>(opcode) & 0x08) != 0;
}

std::string serialize_frame(opcode_t opcode, std::string_view payload, bool fin)
{
  std::string out;
  out.reserve(payload.size() + 10);
  const std::uint8_t b0 = (fin ? 0x80 : 0x00) | static_cast<std::uint8_t>(opcode);
  out.push_back(static_cast<char>(b0));
  const std::size_t len = payload.size();
  if (len < 126)
  {
    out.push_back(static_cast<char>(len));
  }
  else if (len <= 0xffff)
  {
    out.push_back(static_cast<char>(126));
    out.push_back(static_cast<char>((len >> 8) & 0xff));
    out.push_back(static_cast<char>(len & 0xff));
  }
  else
  {
    out.push_back(static_cast<char>(127));
    for (int shift = 56; shift >= 0; shift -= 8)
    {
      out.push_back(static_cast<char>((static_cast<std::uint64_t>(len) >> shift) & 0xff));
    }
  }
  out.append(payload);
  return out;
}

std::string serialize_close(std::uint16_t code, std::string_view reason)
{
  std::string payload;
  payload.push_back(static_cast<char>((code >> 8) & 0xff));
  payload.push_back(static_cast<char>(code & 0xff));
  payload.append(reason);
  return serialize_frame(opcode_t::close, payload, true);
}

frame_status_t frame_reader_t::feed(const char *data, std::size_t length)
{
  if (_failed)
  {
    return frame_status_t::error;
  }
  _buffer.append(data, length);
  while (try_parse_one())
  {
  }
  return _failed ? frame_status_t::error : frame_status_t::ok;
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

bool frame_reader_t::try_parse_one()
{
  if (_failed)
  {
    return false;
  }
  const std::size_t available = _buffer.size();
  if (available < 2)
  {
    return false;
  }
  const auto *bytes = reinterpret_cast<const std::uint8_t *>(_buffer.data());
  const std::uint8_t b0 = bytes[0];
  const std::uint8_t b1 = bytes[1];
  const bool fin = (b0 & 0x80) != 0;
  const std::uint8_t rsv = b0 & 0x70;
  const auto opcode = static_cast<opcode_t>(b0 & 0x0f);
  const bool masked = (b1 & 0x80) != 0;
  std::uint64_t payload_len = b1 & 0x7f;
  std::size_t offset = 2;

  if (rsv != 0)
  {
    _failed = true;
    _close_code = close_code::protocol_error;
    return false;
  }
  switch (opcode)
  {
  case opcode_t::continuation:
  case opcode_t::text:
  case opcode_t::binary:
  case opcode_t::close:
  case opcode_t::ping:
  case opcode_t::pong:
    break;
  default:
    _failed = true;
    _close_code = close_code::protocol_error;
    return false;
  }
  if (is_control(opcode) && (!fin || payload_len > max_control_payload))
  {
    _failed = true;
    _close_code = close_code::protocol_error;
    return false;
  }
  if (!masked)
  {
    _failed = true;
    _close_code = close_code::protocol_error;
    return false;
  }

  if (payload_len == 126)
  {
    if (available < offset + 2)
    {
      return false;
    }
    payload_len = (static_cast<std::uint64_t>(bytes[offset]) << 8) | bytes[offset + 1];
    offset += 2;
  }
  else if (payload_len == 127)
  {
    if (available < offset + 8)
    {
      return false;
    }
    payload_len = 0;
    for (std::size_t i = 0; i < 8; ++i)
    {
      payload_len = (payload_len << 8) | bytes[offset + i];
    }
    offset += 8;
    if ((payload_len >> 63) != 0)
    {
      _failed = true;
      _close_code = close_code::protocol_error;
      return false;
    }
  }
  if (payload_len > _max_frame)
  {
    _failed = true;
    _close_code = close_code::message_too_big;
    return false;
  }

  if (available < offset + 4)
  {
    return false;
  }
  const std::uint8_t mask[4] = {bytes[offset], bytes[offset + 1], bytes[offset + 2], bytes[offset + 3]};
  offset += 4;

  if (available < offset + payload_len)
  {
    return false;
  }

  frame_t frame;
  frame.fin = fin;
  frame.opcode = opcode;
  frame.payload.resize(static_cast<std::size_t>(payload_len));
  for (std::uint64_t i = 0; i < payload_len; ++i)
  {
    frame.payload[static_cast<std::size_t>(i)] = static_cast<char>(bytes[offset + i] ^ mask[i & 3]);
  }
  _buffer.erase(0, offset + static_cast<std::size_t>(payload_len));
  _ready.push_back(std::move(frame));
  return true;
}
} // namespace prism::detail::websocket
