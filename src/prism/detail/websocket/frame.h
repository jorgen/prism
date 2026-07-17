#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>

namespace prism::detail::websocket
{
enum class opcode_t : std::uint8_t
{
  continuation = 0x0,
  text = 0x1,
  binary = 0x2,
  close = 0x8,
  ping = 0x9,
  pong = 0xa,
};

inline constexpr std::size_t max_control_payload = 125;

namespace close_code
{
inline constexpr std::uint16_t normal = 1000;
inline constexpr std::uint16_t going_away = 1001;
inline constexpr std::uint16_t protocol_error = 1002;
inline constexpr std::uint16_t unsupported_data = 1003;
inline constexpr std::uint16_t invalid_payload = 1007;
inline constexpr std::uint16_t policy_violation = 1008;
inline constexpr std::uint16_t message_too_big = 1009;
inline constexpr std::uint16_t internal_error = 1011;
} // namespace close_code

struct frame_t
{
  bool fin = true;
  opcode_t opcode = opcode_t::text;
  std::string payload; // already unmasked
};

enum class frame_status_t : std::uint8_t
{
  ok,
  error,
};

bool is_control(opcode_t opcode);

std::string serialize_frame(opcode_t opcode, std::string_view payload, bool fin = true);
std::string serialize_close(std::uint16_t code, std::string_view reason);

// Incremental parser for inbound (client → server) frames. Client frames are
// always masked; this validates + unmasks them. It emits individual frames
// (including continuation + control frames); message reassembly is the caller's
// job (ws_connection_t) since control frames may interleave data fragments.
class frame_reader_t
{
public:
  frame_status_t feed(const char *data, std::size_t length);
  [[nodiscard]] bool has_frame() const;
  frame_t take_frame();
  [[nodiscard]] std::uint16_t error_close_code() const
  {
    return _close_code;
  }
  void set_max_frame_size(std::size_t bytes)
  {
    _max_frame = bytes;
  }

private:
  bool try_parse_one();

  std::string _buffer;
  std::deque<frame_t> _ready;
  std::size_t _max_frame = 16 * 1024 * 1024;
  bool _failed = false;
  std::uint16_t _close_code = close_code::protocol_error;
};
} // namespace prism::detail::websocket
