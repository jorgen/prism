#include "connection.h"

#include <algorithm>
#include <charconv>
#include <string>
#include <system_error>
#include <utility>

namespace prism::detail::http2
{
namespace
{
constexpr std::int64_t flow_control_max = 0x7fffffff;

bool is_bodyless_status(status_t status)
{
  auto code = static_cast<int>(status);
  return code == 204 || code == 304 || (code >= 100 && code < 200);
}

std::string to_lower(std::string_view value)
{
  std::string out(value);
  for (char &c : out)
  {
    if (c >= 'A' && c <= 'Z')
    {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return out;
}

bool has_uppercase(std::string_view value)
{
  for (char c : value)
  {
    if (c >= 'A' && c <= 'Z')
    {
      return true;
    }
  }
  return false;
}

bool is_connection_specific(std::string_view name)
{
  return name == "connection" || name == "keep-alive" || name == "proxy-connection" || name == "transfer-encoding" || name == "upgrade";
}

std::uint16_t setting_key(settings_id_t id)
{
  return static_cast<std::uint16_t>(id);
}

std::uint32_t read_u32(const char *p)
{
  const unsigned char *b = reinterpret_cast<const unsigned char *>(p);
  return (static_cast<std::uint32_t>(b[0]) << 24) | (static_cast<std::uint32_t>(b[1]) << 16) | (static_cast<std::uint32_t>(b[2]) << 8) | static_cast<std::uint32_t>(b[3]);
}
} // namespace

connection_t::connection_t(const h2_settings_t &local, std::function<bool(method_t, std::string_view)> is_streaming, std::function<bool(std::string_view)> is_websocket)
  : _decoder(local.header_table_size), _local(local), _is_streaming(std::move(is_streaming)), _is_websocket(std::move(is_websocket))
{
  _decoder.set_protocol_max_dynamic_size(_local.header_table_size);
  _decoder.set_max_list_bytes(_local.max_header_list_size);
  _reader.set_max_frame_size(_local.max_frame_size);
}

void connection_t::start()
{
  std::vector<setting_t> settings{
    {setting_key(settings_id_t::header_table_size), _local.header_table_size},
    {setting_key(settings_id_t::max_concurrent_streams), _local.max_concurrent_streams},
    {setting_key(settings_id_t::initial_window_size), _local.initial_window_size},
    {setting_key(settings_id_t::max_frame_size), _local.max_frame_size},
    {setting_key(settings_id_t::max_header_list_size), _local.max_header_list_size},
    {setting_key(settings_id_t::enable_push), 0},
  };
  if (_local.enable_connect_protocol)
  {
    settings.push_back({setting_key(settings_id_t::enable_connect_protocol), 1});
  }
  _out += serialize_settings(settings);
}

stream_t *connection_t::find_stream(std::uint32_t id)
{
  auto it = _streams.find(id);
  return it == _streams.end() ? nullptr : &it->second;
}

bool connection_t::connection_error(error_code_t code)
{
  if (!_failed)
  {
    _conn_error = code;
    _out += serialize_goaway(_last_stream_id, code, {});
    _goaway_sent = true;
    _failed = true;
  }
  return false;
}

void connection_t::queue_stream_reset(std::uint32_t stream_id, error_code_t code)
{
  _out += serialize_rst_stream(stream_id, code);
  if (stream_t *stream = find_stream(stream_id))
  {
    stream->state = stream_state_t::closed;
    stream->body.clear();
    stream->response_body.clear();
    stream->headers.clear();
    stream->response_headers.clear();
  }
}

void connection_t::apply_initial_window_delta(std::int64_t delta)
{
  for (auto &entry : _streams)
  {
    if (entry.second.state != stream_state_t::closed)
    {
      entry.second.send_window += delta;
    }
  }
}

bool connection_t::receive(std::string_view bytes, std::vector<ready_request_t> &ready)
{
  if (_failed)
  {
    return false;
  }
  std::string_view in = bytes;
  if (!_preface_ok)
  {
    while (_preface.size() < connection_preface.size() && !in.empty())
    {
      _preface.push_back(in.front());
      in.remove_prefix(1);
    }
    if (_preface.size() < connection_preface.size())
    {
      return true;
    }
    if (std::string_view(_preface) != connection_preface)
    {
      return connection_error(error_code_t::protocol_error);
    }
    _preface_ok = true;
  }
  if (_reader.feed(in.data(), in.size()) == frame_status_t::error)
  {
    return connection_error(_reader.error());
  }
  while (_reader.has_frame())
  {
    frame_t frame = _reader.take_frame();
    if (!process_frame(frame, ready))
    {
      return false;
    }
  }
  return true;
}

bool connection_t::process_frame(const frame_t &frame, std::vector<ready_request_t> &ready)
{
  if (!_got_first_settings && frame.header.type != frame_type_t::settings)
  {
    return connection_error(error_code_t::protocol_error);
  }
  if (_expect_continuation && frame.header.type != frame_type_t::continuation)
  {
    return connection_error(error_code_t::protocol_error);
  }
  switch (frame.header.type)
  {
  case frame_type_t::settings:
    _got_first_settings = true;
    return on_settings(frame);
  case frame_type_t::headers:
    return on_headers(frame, ready);
  case frame_type_t::continuation:
    return on_continuation(frame, ready);
  case frame_type_t::data:
    return on_data(frame, ready);
  case frame_type_t::window_update:
    return on_window_update(frame);
  case frame_type_t::ping:
    return on_ping(frame);
  case frame_type_t::rst_stream:
    return on_rst_stream(frame);
  case frame_type_t::priority:
    return on_priority(frame);
  case frame_type_t::push_promise:
    return connection_error(error_code_t::protocol_error);
  case frame_type_t::goaway:
    if (frame.header.stream_id != 0)
    {
      return connection_error(error_code_t::protocol_error);
    }
    return true;
  default:
    return true;
  }
}

bool connection_t::on_settings(const frame_t &frame)
{
  if (frame.header.stream_id != 0)
  {
    return connection_error(error_code_t::protocol_error);
  }
  if (frame.header.has_flag(frame_flag::ack))
  {
    if (!frame.payload.empty())
    {
      return connection_error(error_code_t::frame_size_error);
    }
    return true;
  }
  if (frame.payload.size() % 6 != 0)
  {
    return connection_error(error_code_t::frame_size_error);
  }
  ++_pending_settings_ack;
  if (_pending_settings_ack > _local.max_pending_settings_ack)
  {
    return connection_error(error_code_t::enhance_your_calm);
  }
  std::vector<setting_t> settings;
  parse_settings(frame.payload, settings);
  for (const setting_t &s : settings)
  {
    switch (static_cast<settings_id_t>(s.id))
    {
    case settings_id_t::initial_window_size:
      if (s.value > flow_control_max)
      {
        return connection_error(error_code_t::flow_control_error);
      }
      apply_initial_window_delta(static_cast<std::int64_t>(s.value) - static_cast<std::int64_t>(_remote_initial_window));
      _remote_initial_window = s.value;
      break;
    case settings_id_t::max_frame_size:
      if (s.value < default_max_frame_size || s.value > max_allowed_frame_size)
      {
        return connection_error(error_code_t::protocol_error);
      }
      _remote_max_frame = s.value;
      break;
    case settings_id_t::enable_push:
      if (s.value > 1)
      {
        return connection_error(error_code_t::protocol_error);
      }
      break;
    case settings_id_t::enable_connect_protocol:
      if (s.value > 1)
      {
        return connection_error(error_code_t::protocol_error);
      }
      break;
    default:
      break;
    }
  }
  _out += serialize_settings_ack();
  return true;
}

bool connection_t::on_headers(const frame_t &frame, std::vector<ready_request_t> &ready)
{
  std::uint32_t id = frame.header.stream_id;
  if (id == 0 || (id & 1u) == 0)
  {
    return connection_error(error_code_t::protocol_error);
  }

  stream_t *existing = find_stream(id);
  bool trailers = false;
  if (existing != nullptr)
  {
    if (existing->state == stream_state_t::open && existing->headers_complete && !existing->request_delivered)
    {
      trailers = true;
    }
    else if (existing->is_websocket)
    {
      queue_stream_reset(id, error_code_t::protocol_error);
      return true;
    }
    else
    {
      return connection_error(error_code_t::stream_closed);
    }
  }
  else
  {
    if (id <= _last_stream_id)
    {
      return connection_error(error_code_t::protocol_error);
    }
    _last_stream_id = id;
  }

  bool self_dependency = false;
  if (frame.header.has_flag(frame_flag::priority))
  {
    std::size_t offset = frame.header.has_flag(frame_flag::padded) ? 1 : 0;
    if (frame.payload.size() < offset + 5)
    {
      return connection_error(error_code_t::protocol_error);
    }
    std::uint32_t dependency = read_u32(frame.payload.data() + offset) & 0x7fffffff;
    self_dependency = dependency == id;
  }

  bool ok = false;
  std::string_view block = headers_block_fragment(frame.header, frame.payload, ok);
  if (!ok)
  {
    return connection_error(error_code_t::protocol_error);
  }

  if (!trailers)
  {
    stream_t &stream = _streams[id];
    stream.id = id;
    stream.state = frame.header.has_flag(frame_flag::end_stream) ? stream_state_t::half_closed_remote : stream_state_t::open;
    stream.send_window = static_cast<std::int64_t>(_remote_initial_window);
    stream.recv_window = static_cast<std::int64_t>(_local.initial_window_size);
    ++_streams_opened;
    if (_local.max_total_streams != 0 && _streams_opened > _local.max_total_streams)
    {
      return connection_error(error_code_t::enhance_your_calm);
    }
    if (_goaway_sent || active_streams() > _local.max_concurrent_streams || self_dependency)
    {
      queue_stream_reset(id, self_dependency ? error_code_t::protocol_error : error_code_t::refused_stream);
    }
  }
  else if (self_dependency)
  {
    queue_stream_reset(id, error_code_t::protocol_error);
  }

  bool end_stream = frame.header.has_flag(frame_flag::end_stream);
  if (frame.header.has_flag(frame_flag::end_headers))
  {
    return finish_header_block(id, block, end_stream, ready);
  }
  _expect_continuation = true;
  _continuation_stream = id;
  _continuation_end_stream = end_stream;
  _header_block.assign(block);
  if (_header_block.size() > _local.max_header_list_size)
  {
    return connection_error(error_code_t::enhance_your_calm);
  }
  return true;
}

bool connection_t::on_continuation(const frame_t &frame, std::vector<ready_request_t> &ready)
{
  if (!_expect_continuation || frame.header.stream_id != _continuation_stream)
  {
    return connection_error(error_code_t::protocol_error);
  }
  _header_block.append(frame.payload);
  if (_header_block.size() > _local.max_header_list_size)
  {
    return connection_error(error_code_t::enhance_your_calm);
  }
  if (frame.header.has_flag(frame_flag::end_headers))
  {
    return finish_header_block(_continuation_stream, _header_block, _continuation_end_stream, ready);
  }
  return true;
}

bool connection_t::finish_header_block(std::uint32_t stream_id, std::string_view block, bool end_stream, std::vector<ready_request_t> &ready)
{
  std::vector<hpack_header_t> decoded;
  bool decoded_ok = _decoder.decode(block, decoded);
  _expect_continuation = false;
  _header_block.clear();
  if (!decoded_ok)
  {
    return connection_error(error_code_t::compression_error);
  }
  stream_t *stream = find_stream(stream_id);
  if (stream == nullptr || stream->state == stream_state_t::closed)
  {
    return true;
  }
  if (stream->headers_complete)
  {
    if (!end_stream)
    {
      queue_stream_reset(stream_id, error_code_t::protocol_error);
      return true;
    }
    for (const hpack_header_t &header : decoded)
    {
      if (!header.name.empty() && header.name.front() == ':')
      {
        queue_stream_reset(stream_id, error_code_t::protocol_error);
        return true;
      }
    }
    stream->state = stream_state_t::half_closed_remote;
    return deliver_request(*stream, ready);
  }
  stream->headers = std::move(decoded);
  stream->headers_complete = true;
  {
    std::string method;
    std::string path;
    std::string protocol;
    for (const hpack_header_t &header : stream->headers)
    {
      if (header.name == ":method")
      {
        method = header.value;
      }
      else if (header.name == ":path")
      {
        path = header.value;
      }
      else if (header.name == ":protocol")
      {
        protocol = header.value;
      }
    }
    std::size_t query = path.find('?');
    std::string_view path_component = query == std::string::npos ? std::string_view(path) : std::string_view(path).substr(0, query);
    if (method == "CONNECT")
    {
      stream->request_streaming = true;
      if (_local.enable_connect_protocol && protocol == "websocket" && _is_websocket && _is_websocket(path_component))
      {
        stream->is_websocket = true;
      }
    }
    else if (_is_streaming && _is_streaming(method_from_name(method), path_component))
    {
      stream->request_streaming = true;
    }
  }
  if (end_stream)
  {
    stream->state = stream_state_t::half_closed_remote;
    stream->inbound_ended = true;
  }
  if (stream->request_streaming || end_stream)
  {
    return deliver_request(*stream, ready);
  }
  return true;
}

bool connection_t::deliver_request(stream_t &stream, std::vector<ready_request_t> &ready)
{
  if (stream.request_delivered)
  {
    return true;
  }
  request_t request;
  std::string method;
  std::string path;
  std::string scheme;
  std::string authority;
  std::string protocol;
  bool have_method = false;
  bool have_path = false;
  bool have_scheme = false;
  bool have_authority = false;
  bool have_protocol = false;
  bool seen_regular = false;
  bool have_content_length = false;
  std::size_t content_length = 0;
  for (const hpack_header_t &header : stream.headers)
  {
    if (!header.name.empty() && header.name.front() == ':')
    {
      if (seen_regular)
      {
        queue_stream_reset(stream.id, error_code_t::protocol_error);
        return true;
      }
      if (header.name == ":method")
      {
        if (have_method)
        {
          queue_stream_reset(stream.id, error_code_t::protocol_error);
          return true;
        }
        method = header.value;
        have_method = true;
      }
      else if (header.name == ":path")
      {
        if (have_path)
        {
          queue_stream_reset(stream.id, error_code_t::protocol_error);
          return true;
        }
        path = header.value;
        have_path = true;
      }
      else if (header.name == ":scheme")
      {
        if (have_scheme)
        {
          queue_stream_reset(stream.id, error_code_t::protocol_error);
          return true;
        }
        scheme = header.value;
        have_scheme = true;
      }
      else if (header.name == ":authority")
      {
        if (have_authority)
        {
          queue_stream_reset(stream.id, error_code_t::protocol_error);
          return true;
        }
        authority = header.value;
        have_authority = true;
      }
      else if (_local.enable_connect_protocol && header.name == ":protocol")
      {
        if (have_protocol)
        {
          queue_stream_reset(stream.id, error_code_t::protocol_error);
          return true;
        }
        protocol = header.value;
        have_protocol = true;
      }
      else
      {
        queue_stream_reset(stream.id, error_code_t::protocol_error);
        return true;
      }
    }
    else
    {
      seen_regular = true;
      if (has_uppercase(header.name) || is_connection_specific(header.name))
      {
        queue_stream_reset(stream.id, error_code_t::protocol_error);
        return true;
      }
      if (header.name == "te" && header.value != "trailers")
      {
        queue_stream_reset(stream.id, error_code_t::protocol_error);
        return true;
      }
      if (header.name == "content-length")
      {
        std::size_t parsed = 0;
        auto begin = header.value.data();
        auto end = begin + header.value.size();
        auto [ptr, ec] = std::from_chars(begin, end, parsed);
        if (ec != std::errc{} || ptr != end)
        {
          queue_stream_reset(stream.id, error_code_t::protocol_error);
          return true;
        }
        have_content_length = true;
        content_length = parsed;
      }
      request.headers.entries.push_back({header.name, header.value});
    }
  }
  if (method.empty() || path.empty() || scheme.empty())
  {
    queue_stream_reset(stream.id, error_code_t::protocol_error);
    return true;
  }
  request.method = method_from_name(method);
  request.target = path;
  std::size_t query = path.find('?');
  request.path = query == std::string::npos ? path : path.substr(0, query);
  if (!authority.empty())
  {
    request.headers.set("host", authority);
  }
  if (method == "CONNECT")
  {
    const bool is_ws = have_protocol && protocol == "websocket" && !authority.empty() && _is_websocket && _is_websocket(request.path);
    if (!is_ws)
    {
      queue_stream_reset(stream.id, error_code_t::refused_stream);
      return true;
    }
    request.method = method_t::get;
    stream.is_websocket = true;
  }
  stream.head = method == "HEAD";
  stream.request_delivered = true;

  if (stream.request_streaming)
  {
    if (have_content_length)
    {
      stream.content_length = content_length;
    }
    if (stream.is_websocket)
    {
      ready.push_back(ready_request_t{stream.id, std::move(request), false, false, true});
    }
    else
    {
      ready.push_back(ready_request_t{stream.id, std::move(request), stream.head, true});
    }
    return true;
  }

  if (have_content_length && content_length != stream.body.size())
  {
    queue_stream_reset(stream.id, error_code_t::protocol_error);
    return true;
  }
  request.body = std::move(stream.body);
  ready.push_back(ready_request_t{stream.id, std::move(request), stream.head});
  return true;
}

bool connection_t::on_data(const frame_t &frame, std::vector<ready_request_t> &ready)
{
  std::uint32_t id = frame.header.stream_id;
  if (id == 0)
  {
    return connection_error(error_code_t::protocol_error);
  }
  if (id > _last_stream_id)
  {
    return connection_error(error_code_t::protocol_error);
  }
  stream_t *stream = find_stream(id);
  if (stream == nullptr || stream->state == stream_state_t::closed)
  {
    return connection_error(error_code_t::stream_closed);
  }

  bool ok = false;
  std::string_view data = data_without_padding(frame.header, frame.payload, ok);
  if (!ok)
  {
    return connection_error(error_code_t::protocol_error);
  }
  std::int64_t framed = static_cast<std::int64_t>(frame.header.length);
  _conn_recv_window -= framed;
  if (_conn_recv_window < 0)
  {
    return connection_error(error_code_t::flow_control_error);
  }

  if (stream->state != stream_state_t::open)
  {
    _conn_recv_window += framed;
    if (framed > 0)
    {
      _out += serialize_window_update(0, static_cast<std::uint32_t>(framed));
    }
    queue_stream_reset(id, error_code_t::stream_closed);
    return true;
  }

  bool end_stream = frame.header.has_flag(frame_flag::end_stream);

  if (!stream->request_streaming)
  {
    // Buffered route: replenish immediately (byte-identical to the pre-streaming
    // behaviour, keeping h2spec's flow-control assertions satisfied).
    _conn_recv_window += framed;
    if (framed > 0)
    {
      _out += serialize_window_update(0, static_cast<std::uint32_t>(framed));
    }
    stream->body.append(data);
    if (stream->body.size() > _local.max_body_bytes)
    {
      queue_stream_reset(id, error_code_t::enhance_your_calm);
      return true;
    }
    if (!end_stream)
    {
      if (framed > 0)
      {
        _out += serialize_window_update(id, static_cast<std::uint32_t>(framed));
      }
      return true;
    }
    stream->state = stream_state_t::half_closed_remote;
    if (stream->headers_complete)
    {
      return deliver_request(*stream, ready);
    }
    return true;
  }

  // Streaming route (already delivered at END_HEADERS): consume-driven flow
  // control. Do NOT replenish here -- the reader calls inbound_consume() as the
  // handler takes bytes, which is the backpressure lever.
  stream->recv_window -= framed;
  if (stream->recv_window < 0)
  {
    stream->inbound_aborted = true;
    queue_stream_reset(id, error_code_t::flow_control_error);
    return true;
  }
  stream->inbound_total += data.size();
  if (stream->content_length.has_value() && stream->inbound_total > *stream->content_length)
  {
    stream->inbound_aborted = true;
    queue_stream_reset(id, error_code_t::protocol_error);
    return true;
  }
  if (!data.empty())
  {
    stream->inbound_chunks.emplace_back(data);
  }
  if (end_stream)
  {
    if (stream->content_length.has_value() && stream->inbound_total != *stream->content_length)
    {
      stream->inbound_aborted = true;
      queue_stream_reset(id, error_code_t::protocol_error);
      return true;
    }
    stream->inbound_ended = true;
    stream->state = stream_state_t::half_closed_remote;
  }
  return true;
}

bool connection_t::on_window_update(const frame_t &frame)
{
  std::uint32_t increment = 0;
  if (!parse_window_update(frame.payload, increment))
  {
    return connection_error(error_code_t::frame_size_error);
  }
  if (increment == 0)
  {
    if (frame.header.stream_id == 0)
    {
      return connection_error(error_code_t::protocol_error);
    }
    queue_stream_reset(frame.header.stream_id, error_code_t::protocol_error);
    return true;
  }
  if (frame.header.stream_id == 0)
  {
    _conn_send_window += static_cast<std::int64_t>(increment);
    if (_conn_send_window > flow_control_max)
    {
      return connection_error(error_code_t::flow_control_error);
    }
    return true;
  }
  if (frame.header.stream_id > _last_stream_id)
  {
    return connection_error(error_code_t::protocol_error);
  }
  stream_t *stream = find_stream(frame.header.stream_id);
  if (stream != nullptr && stream->state != stream_state_t::closed)
  {
    stream->send_window += static_cast<std::int64_t>(increment);
    if (stream->send_window > flow_control_max)
    {
      queue_stream_reset(frame.header.stream_id, error_code_t::flow_control_error);
    }
  }
  return true;
}

bool connection_t::on_ping(const frame_t &frame)
{
  if (frame.header.stream_id != 0)
  {
    return connection_error(error_code_t::protocol_error);
  }
  if (frame.payload.size() != ping_payload_size)
  {
    return connection_error(error_code_t::frame_size_error);
  }
  if (frame.header.has_flag(frame_flag::ack))
  {
    return true;
  }
  _out += serialize_ping(frame.payload, true);
  return true;
}

bool connection_t::on_rst_stream(const frame_t &frame)
{
  std::uint32_t id = frame.header.stream_id;
  if (id == 0)
  {
    return connection_error(error_code_t::protocol_error);
  }
  if (frame.payload.size() != 4)
  {
    return connection_error(error_code_t::frame_size_error);
  }
  if (id > _last_stream_id)
  {
    return connection_error(error_code_t::protocol_error);
  }
  ++_reset_received;
  if (_reset_received > _local.max_reset_streams)
  {
    return connection_error(error_code_t::enhance_your_calm);
  }
  if (stream_t *stream = find_stream(id))
  {
    stream->state = stream_state_t::closed;
    stream->body.clear();
    stream->response_body.clear();
    stream->headers.clear();
    stream->response_headers.clear();
    stream->inbound_aborted = true;
    stream->inbound_chunks.clear();
  }
  return true;
}

bool connection_t::on_priority(const frame_t &frame)
{
  if (frame.header.stream_id == 0)
  {
    return connection_error(error_code_t::protocol_error);
  }
  if (frame.payload.size() != 5)
  {
    queue_stream_reset(frame.header.stream_id, error_code_t::frame_size_error);
    return true;
  }
  std::uint32_t dependency = read_u32(frame.payload.data()) & 0x7fffffff;
  if (dependency == frame.header.stream_id)
  {
    queue_stream_reset(frame.header.stream_id, error_code_t::protocol_error);
  }
  return true;
}

bool connection_t::inbound_has_chunk(std::uint32_t stream_id)
{
  stream_t *stream = find_stream(stream_id);
  return stream != nullptr && !stream->inbound_chunks.empty();
}

bool connection_t::inbound_ended(std::uint32_t stream_id)
{
  stream_t *stream = find_stream(stream_id);
  return stream == nullptr || stream->inbound_ended;
}

bool connection_t::inbound_aborted(std::uint32_t stream_id)
{
  stream_t *stream = find_stream(stream_id);
  return stream == nullptr || stream->inbound_aborted;
}

bool connection_t::take_inbound_chunk(std::uint32_t stream_id, std::string &data_out, bool &last_out)
{
  stream_t *stream = find_stream(stream_id);
  if (stream == nullptr || stream->inbound_chunks.empty())
  {
    return false;
  }
  data_out = std::move(stream->inbound_chunks.front());
  stream->inbound_chunks.pop_front();
  last_out = stream->inbound_chunks.empty() && stream->inbound_ended;
  return true;
}

void connection_t::inbound_consume(std::uint32_t stream_id, std::size_t bytes)
{
  if (bytes == 0)
  {
    return;
  }
  std::int64_t delta = static_cast<std::int64_t>(bytes);
  _conn_recv_window += delta;
  if (_conn_recv_window > flow_control_max)
  {
    _conn_recv_window = flow_control_max;
  }
  _out += serialize_window_update(0, static_cast<std::uint32_t>(bytes));
  stream_t *stream = find_stream(stream_id);
  if (stream != nullptr && stream->state != stream_state_t::closed)
  {
    stream->recv_window += delta;
    if (stream->recv_window > flow_control_max)
    {
      stream->recv_window = flow_control_max;
    }
    _out += serialize_window_update(stream_id, static_cast<std::uint32_t>(bytes));
  }
}

void connection_t::set_inbound_waiter(std::uint32_t stream_id, std::coroutine_handle<> handle)
{
  stream_t *stream = find_stream(stream_id);
  if (stream != nullptr)
  {
    stream->inbound_waiter = handle;
  }
}

void connection_t::wake_inbound(std::uint32_t stream_id)
{
  stream_t *stream = find_stream(stream_id);
  if (stream != nullptr && stream->inbound_waiter)
  {
    std::coroutine_handle<> handle = stream->inbound_waiter;
    stream->inbound_waiter = {};
    handle.resume();
  }
}

std::optional<std::size_t> connection_t::inbound_length(std::uint32_t stream_id)
{
  stream_t *stream = find_stream(stream_id);
  if (stream == nullptr)
  {
    return std::nullopt;
  }
  return stream->content_length;
}

void connection_t::abort_inbound(std::uint32_t stream_id, error_code_t code)
{
  stream_t *stream = find_stream(stream_id);
  if (stream == nullptr || stream->state == stream_state_t::closed)
  {
    return;
  }
  queue_stream_reset(stream_id, code);
}

void connection_t::collect_inbound_ready(std::vector<std::coroutine_handle<>> &out)
{
  for (auto &entry : _streams)
  {
    stream_t &stream = entry.second;
    if (stream.inbound_waiter && (!stream.inbound_chunks.empty() || stream.inbound_ended || stream.inbound_aborted))
    {
      out.push_back(stream.inbound_waiter);
      stream.inbound_waiter = {};
    }
  }
}

void connection_t::fail_all_inbound()
{
  for (auto &entry : _streams)
  {
    entry.second.inbound_aborted = true;
  }
}

namespace
{
void fill_response_headers(std::vector<hpack_header_t> &out, const response_t &response)
{
  out.clear();
  out.push_back({":status", std::to_string(static_cast<int>(response.status))});
  for (const header_t &header : response.headers.entries)
  {
    std::string name = to_lower(header.name);
    if (name.empty() || name.front() == ':' || is_connection_specific(name) || name == "content-length")
    {
      continue;
    }
    out.push_back({name, header.value});
  }
}
} // namespace

void connection_t::submit_response(std::uint32_t stream_id, response_t response, bool head)
{
  stream_t *stream = find_stream(stream_id);
  if (stream == nullptr || stream->state == stream_state_t::closed)
  {
    return;
  }
  stream->has_response = true;
  stream->head = head || stream->head;
  stream->response_status = response.status;
  fill_response_headers(stream->response_headers, response);
  stream->response_body = std::move(response.body);
  if (!is_bodyless_status(response.status))
  {
    stream->response_headers.push_back({"content-length", std::to_string(stream->response_body.size())});
  }
  stream->response_offset = 0;
}

void connection_t::begin_streaming_response(std::uint32_t stream_id, const response_t &response, bool head)
{
  stream_t *stream = find_stream(stream_id);
  if (stream == nullptr || stream->state == stream_state_t::closed)
  {
    return;
  }
  stream->has_response = true;
  stream->streaming = true;
  stream->head = head || stream->head;
  stream->response_status = response.status;
  fill_response_headers(stream->response_headers, response);
  stream->response_body.clear();
  stream->response_offset = 0;
}

void connection_t::push_stream_data(std::uint32_t stream_id, std::string_view data, bool last)
{
  stream_t *stream = find_stream(stream_id);
  if (stream == nullptr || stream->state == stream_state_t::closed)
  {
    return;
  }
  stream->response_body.append(data);
  if (last)
  {
    stream->stream_ended = true;
  }
}

std::size_t connection_t::stream_send_pending(std::uint32_t stream_id)
{
  stream_t *stream = find_stream(stream_id);
  if (stream == nullptr || stream->state == stream_state_t::closed)
  {
    return 0;
  }
  return stream->response_body.size() - stream->response_offset;
}

void connection_t::reset_stream(std::uint32_t stream_id, error_code_t code)
{
  queue_stream_reset(stream_id, code);
}

void connection_t::emit_headers(std::uint32_t id, std::string_view block, bool end_stream)
{
  std::size_t max = _remote_max_frame;
  if (block.size() <= max)
  {
    _out += serialize_headers(id, block, end_stream, true);
    return;
  }
  _out += serialize_headers(id, block.substr(0, max), end_stream, false);
  std::size_t offset = max;
  while (offset < block.size())
  {
    std::size_t chunk = std::min(max, block.size() - offset);
    bool last = offset + chunk == block.size();
    _out += serialize_continuation(id, block.substr(offset, chunk), last);
    offset += chunk;
  }
}

void connection_t::pump()
{
  if (_failed)
  {
    return;
  }
  for (auto &entry : _streams)
  {
    stream_t &stream = entry.second;
    if (!stream.has_response || stream.response_done)
    {
      continue;
    }
    if (!stream.response_headers_sent)
    {
      std::string block = _encoder.encode(stream.response_headers);
      bool no_body = is_bodyless_status(stream.response_status) || stream.head || (!stream.streaming && stream.response_body.empty());
      emit_headers(stream.id, block, no_body);
      stream.response_headers_sent = true;
      if (no_body)
      {
        stream.response_done = true;
        stream.state = stream_state_t::closed;
        stream.response_body.clear();
        continue;
      }
    }
    while (stream.response_offset < stream.response_body.size())
    {
      std::int64_t available = std::min(_conn_send_window, stream.send_window);
      if (available <= 0)
      {
        break;
      }
      std::int64_t chunk = std::min(available, static_cast<std::int64_t>(_remote_max_frame));
      std::int64_t remaining = static_cast<std::int64_t>(stream.response_body.size() - stream.response_offset);
      chunk = std::min(chunk, remaining);
      bool at_end = stream.response_offset + static_cast<std::size_t>(chunk) == stream.response_body.size();
      bool last = at_end && (!stream.streaming || stream.stream_ended);
      std::string_view slice = std::string_view(stream.response_body).substr(stream.response_offset, static_cast<std::size_t>(chunk));
      _out += serialize_data(stream.id, slice, last);
      stream.response_offset += static_cast<std::size_t>(chunk);
      _conn_send_window -= chunk;
      stream.send_window -= chunk;
      if (last)
      {
        stream.response_done = true;
        stream.state = stream_state_t::closed;
        stream.response_body.clear();
      }
    }
    if (stream.streaming && stream.stream_ended && !stream.response_done && stream.response_offset == stream.response_body.size())
    {
      _out += serialize_data(stream.id, {}, true);
      stream.response_done = true;
      stream.state = stream_state_t::closed;
      stream.response_body.clear();
    }
  }
}

bool connection_t::has_output()
{
  pump();
  return !_out.empty();
}

std::string connection_t::take_output()
{
  pump();
  std::string out = std::move(_out);
  _out.clear();
  return out;
}

void connection_t::begin_goaway(error_code_t code)
{
  if (!_goaway_sent)
  {
    _out += serialize_goaway(_last_stream_id, code, {});
    _goaway_sent = true;
  }
}

bool connection_t::wants_close() const
{
  return _failed || (_goaway_sent && active_streams() == 0);
}

bool connection_t::failed() const
{
  return _failed;
}

error_code_t connection_t::connection_error() const
{
  return _conn_error;
}

std::size_t connection_t::active_streams() const
{
  std::size_t count = 0;
  for (const auto &entry : _streams)
  {
    if (entry.second.state != stream_state_t::closed)
    {
      ++count;
    }
  }
  return count;
}

std::uint32_t connection_t::streams_opened() const
{
  return _streams_opened;
}
} // namespace prism::detail::http2
