#include "prism/detail/http1.h"

#include <charconv>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <deque>
#include <string_view>
#include <utility>

#include <llhttp.h>

namespace prism::detail
{
struct codec_state_t
{
  llhttp_t parser{};
  parsed_request_t current;
  std::string cur_field;
  std::string cur_value;
  std::deque<parsed_request_t> ready;
  status_t error = status_t::bad_request;
  std::size_t body_bytes = 0;
  std::size_t header_bytes = 0;
  bool in_progress = false;
  bool headers_complete = false;
};

namespace
{
constexpr std::size_t max_body_bytes = 16u * 1024u * 1024u;
constexpr std::size_t max_header_bytes = 64u * 1024u;

codec_state_t *state_of(llhttp_t *parser)
{
  return static_cast<codec_state_t *>(parser->data);
}

bool iequals(std::string_view a, std::string_view b)
{
  if (a.size() != b.size())
  {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i)
  {
    unsigned char ca = static_cast<unsigned char>(a[i]);
    unsigned char cb = static_cast<unsigned char>(b[i]);
    if (ca >= 'A' && ca <= 'Z')
    {
      ca = static_cast<unsigned char>(ca - 'A' + 'a');
    }
    if (cb >= 'A' && cb <= 'Z')
    {
      cb = static_cast<unsigned char>(cb - 'A' + 'a');
    }
    if (ca != cb)
    {
      return false;
    }
  }
  return true;
}

std::string http_date()
{
  std::time_t now = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &now);
#else
  gmtime_r(&now, &tm);
#endif
  char buf[64];
  std::size_t n = std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm);
  return std::string(buf, n);
}

int on_message_begin(llhttp_t *parser)
{
  codec_state_t *state = state_of(parser);
  state->current = parsed_request_t{};
  state->cur_field.clear();
  state->cur_value.clear();
  state->body_bytes = 0;
  state->header_bytes = 0;
  state->in_progress = true;
  state->headers_complete = false;
  return 0;
}

int on_url(llhttp_t *parser, const char *at, std::size_t length)
{
  state_of(parser)->current.request.target.append(at, length);
  return 0;
}

int on_url_complete(llhttp_t *parser)
{
  codec_state_t *state = state_of(parser);
  const std::string &target = state->current.request.target;
  std::size_t query = target.find('?');
  state->current.request.path = query == std::string::npos ? target : target.substr(0, query);
  return 0;
}

int on_header_field(llhttp_t *parser, const char *at, std::size_t length)
{
  codec_state_t *state = state_of(parser);
  state->header_bytes += length;
  if (state->header_bytes > max_header_bytes)
  {
    state->error = status_t::request_header_fields_too_large;
    return -1;
  }
  state->cur_field.append(at, length);
  return 0;
}

int on_header_value(llhttp_t *parser, const char *at, std::size_t length)
{
  codec_state_t *state = state_of(parser);
  state->header_bytes += length;
  if (state->header_bytes > max_header_bytes)
  {
    state->error = status_t::request_header_fields_too_large;
    return -1;
  }
  state->cur_value.append(at, length);
  return 0;
}

int on_header_value_complete(llhttp_t *parser)
{
  codec_state_t *state = state_of(parser);
  state->current.request.headers.entries.push_back(header_t{std::move(state->cur_field), std::move(state->cur_value)});
  state->cur_field.clear();
  state->cur_value.clear();
  return 0;
}

int on_headers_complete(llhttp_t *parser)
{
  state_of(parser)->headers_complete = true;
  return 0;
}

int on_body(llhttp_t *parser, const char *at, std::size_t length)
{
  codec_state_t *state = state_of(parser);
  state->body_bytes += length;
  if (state->body_bytes > max_body_bytes)
  {
    state->error = status_t::payload_too_large;
    return -1;
  }
  state->current.request.body.append(at, length);
  return 0;
}

int on_message_complete(llhttp_t *parser)
{
  codec_state_t *state = state_of(parser);
  const char *name = llhttp_method_name(static_cast<llhttp_method_t>(llhttp_get_method(parser)));
  state->current.request.method = method_from_name(name != nullptr ? name : "");
  state->current.keep_alive = llhttp_should_keep_alive(parser) != 0;
  state->ready.push_back(std::move(state->current));
  state->current = parsed_request_t{};
  state->in_progress = false;
  return 0;
}

const llhttp_settings_t &settings()
{
  static const llhttp_settings_t value = []
  {
    llhttp_settings_t cfg;
    llhttp_settings_init(&cfg);
    cfg.on_message_begin = on_message_begin;
    cfg.on_url = on_url;
    cfg.on_url_complete = on_url_complete;
    cfg.on_header_field = on_header_field;
    cfg.on_header_value = on_header_value;
    cfg.on_header_value_complete = on_header_value_complete;
    cfg.on_headers_complete = on_headers_complete;
    cfg.on_body = on_body;
    cfg.on_message_complete = on_message_complete;
    return cfg;
  }();
  return value;
}
} // namespace

request_codec_t::request_codec_t()
  : _impl(std::make_unique<codec_state_t>())
{
  llhttp_init(&_impl->parser, HTTP_REQUEST, &settings());
  _impl->parser.data = _impl.get();
}

request_codec_t::~request_codec_t() = default;
request_codec_t::request_codec_t(request_codec_t &&) noexcept = default;
request_codec_t &request_codec_t::operator=(request_codec_t &&) noexcept = default;

feed_result_t request_codec_t::feed(const char *data, std::size_t length)
{
  if (llhttp_execute(&_impl->parser, data, length) == HPE_OK)
  {
    return feed_result_t::ok;
  }
  return feed_result_t::error;
}

void request_codec_t::finish()
{
  llhttp_finish(&_impl->parser);
}

bool request_codec_t::has_request() const
{
  return !_impl->ready.empty();
}

parsed_request_t request_codec_t::take_request()
{
  parsed_request_t request = std::move(_impl->ready.front());
  _impl->ready.pop_front();
  return request;
}

status_t request_codec_t::error_status() const
{
  return _impl->error;
}

bool request_codec_t::current_in_progress() const
{
  return _impl->in_progress;
}

bool request_codec_t::current_headers_complete() const
{
  return _impl->headers_complete;
}

std::string serialize_response(const response_t &response, bool keep_alive, bool head_request)
{
  std::string out;
  out.reserve(response.body.size() + 256);

  out += "HTTP/1.1 ";
  out += std::to_string(static_cast<int>(response.status));
  out += ' ';
  out.append(reason_phrase(response.status));
  out += "\r\n";

  bool has_date = false;
  bool has_server = false;
  for (const auto &entry : response.headers.entries)
  {
    if (iequals(entry.name, "content-length") || iequals(entry.name, "connection") || iequals(entry.name, "transfer-encoding"))
    {
      continue;
    }
    if (iequals(entry.name, "date"))
    {
      has_date = true;
    }
    if (iequals(entry.name, "server"))
    {
      has_server = true;
    }
    out += entry.name;
    out += ": ";
    out += entry.value;
    out += "\r\n";
  }

  const int status_code = static_cast<int>(response.status);
  const bool bodyless_status = status_code == 204 || status_code == 304 || (status_code >= 100 && status_code < 200);
  if (!bodyless_status)
  {
    out += "Content-Length: ";
    out += std::to_string(response.body.size());
    out += "\r\n";
  }
  out += "Connection: ";
  out += keep_alive ? "keep-alive" : "close";
  out += "\r\n";
  if (!has_date)
  {
    out += "Date: ";
    out += http_date();
    out += "\r\n";
  }
  if (!has_server)
  {
    out += "Server: prism\r\n";
  }
  out += "\r\n";
  if (!head_request && !bodyless_status)
  {
    out += response.body;
  }
  return out;
}

std::string serialize_streaming_headers(const response_t &response, bool keep_alive)
{
  std::string out;
  out.reserve(256);

  out += "HTTP/1.1 ";
  out += std::to_string(static_cast<int>(response.status));
  out += ' ';
  out.append(reason_phrase(response.status));
  out += "\r\n";

  bool has_date = false;
  bool has_server = false;
  for (const auto &entry : response.headers.entries)
  {
    if (iequals(entry.name, "content-length") || iequals(entry.name, "connection") || iequals(entry.name, "transfer-encoding"))
    {
      continue;
    }
    if (iequals(entry.name, "date"))
    {
      has_date = true;
    }
    if (iequals(entry.name, "server"))
    {
      has_server = true;
    }
    out += entry.name;
    out += ": ";
    out += entry.value;
    out += "\r\n";
  }

  out += "Transfer-Encoding: chunked\r\n";
  out += "Connection: ";
  out += keep_alive ? "keep-alive" : "close";
  out += "\r\n";
  if (!has_date)
  {
    out += "Date: ";
    out += http_date();
    out += "\r\n";
  }
  if (!has_server)
  {
    out += "Server: prism\r\n";
  }
  out += "\r\n";
  return out;
}

std::string serialize_chunk(std::string_view data)
{
  if (data.empty())
  {
    return {};
  }
  char size_hex[20];
  auto [ptr, ec] = std::to_chars(size_hex, size_hex + sizeof(size_hex), data.size(), 16);
  std::string out;
  out.reserve(data.size() + 20);
  out.append(size_hex, ptr);
  out += "\r\n";
  out.append(data);
  out += "\r\n";
  return out;
}

std::string serialize_last_chunk()
{
  return "0\r\n\r\n";
}
} // namespace prism::detail
