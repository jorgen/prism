#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "../../http.h"
#include "frame.h"
#include "hpack.h"

namespace prism::detail::http2
{
struct h2_settings_t
{
  std::uint32_t header_table_size = 4096;
  std::uint32_t max_concurrent_streams = 128;
  std::uint32_t initial_window_size = 65535;
  std::uint32_t max_frame_size = 16384;
  std::uint32_t max_header_list_size = 65536;
  std::size_t max_body_bytes = std::size_t{16} * 1024 * 1024;
  std::uint32_t max_reset_streams = 200;
  std::uint32_t max_pending_settings_ack = 8;
};

struct ready_request_t
{
  std::uint32_t stream_id = 0;
  request_t request;
  bool head = false;
};

enum class stream_state_t : std::uint8_t
{
  idle,
  open,
  half_closed_remote,
  half_closed_local,
  closed,
};

struct stream_t
{
  std::uint32_t id = 0;
  stream_state_t state = stream_state_t::idle;

  std::vector<hpack_header_t> headers;
  std::string body;
  bool headers_complete = false;
  bool request_delivered = false;
  bool head = false;

  bool has_response = false;
  bool response_headers_sent = false;
  bool response_done = false;
  status_t response_status = status_t::ok;
  std::vector<hpack_header_t> response_headers;
  std::string response_body;
  std::size_t response_offset = 0;

  std::int64_t send_window = 0;
};

class connection_t
{
public:
  explicit connection_t(const h2_settings_t &local = {});

  void start();

  bool receive(std::string_view bytes, std::vector<ready_request_t> &ready);

  void submit_response(std::uint32_t stream_id, response_t response, bool head);
  void reset_stream(std::uint32_t stream_id, error_code_t code);

  [[nodiscard]] bool has_output();
  std::string take_output();

  void begin_goaway(error_code_t code = error_code_t::no_error);

  [[nodiscard]] bool wants_close() const;
  [[nodiscard]] bool failed() const;
  [[nodiscard]] error_code_t connection_error() const;
  [[nodiscard]] std::size_t active_streams() const;
  [[nodiscard]] std::uint32_t streams_opened() const;

private:
  bool process_frame(const frame_t &frame, std::vector<ready_request_t> &ready);
  bool on_headers(const frame_t &frame, std::vector<ready_request_t> &ready);
  bool on_continuation(const frame_t &frame, std::vector<ready_request_t> &ready);
  bool on_data(const frame_t &frame, std::vector<ready_request_t> &ready);
  bool on_settings(const frame_t &frame);
  bool on_window_update(const frame_t &frame);
  bool on_ping(const frame_t &frame);
  bool on_rst_stream(const frame_t &frame);
  bool on_priority(const frame_t &frame);

  bool finish_header_block(std::uint32_t stream_id, std::string_view block, bool end_stream, std::vector<ready_request_t> &ready);
  bool deliver_request(stream_t &stream, std::vector<ready_request_t> &ready);

  void pump();
  void emit_headers(std::uint32_t id, std::string_view block, bool end_stream);
  bool connection_error(error_code_t code);
  void queue_stream_reset(std::uint32_t stream_id, error_code_t code);
  void apply_initial_window_delta(std::int64_t delta);

  stream_t *find_stream(std::uint32_t id);

  frame_reader_t _reader;
  hpack_decoder_t _decoder;
  hpack_encoder_t _encoder;
  std::map<std::uint32_t, stream_t> _streams;
  std::string _out;
  h2_settings_t _local;

  std::string _preface;
  bool _preface_ok = false;
  bool _got_first_settings = false;

  std::int64_t _conn_send_window = 65535;
  std::int64_t _conn_recv_window = 65535;
  std::uint32_t _remote_initial_window = 65535;
  std::uint32_t _remote_max_frame = 16384;

  std::uint32_t _last_stream_id = 0;
  std::uint32_t _highest_processed = 0;
  std::uint32_t _streams_opened = 0;
  std::uint32_t _reset_received = 0;
  std::uint32_t _pending_settings_ack = 0;

  bool _expect_continuation = false;
  std::uint32_t _continuation_stream = 0;
  bool _continuation_end_stream = false;
  std::string _header_block;

  bool _goaway_sent = false;
  bool _failed = false;
  error_code_t _conn_error = error_code_t::no_error;
};
} // namespace prism::detail::http2
