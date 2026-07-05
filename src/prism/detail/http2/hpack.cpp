#include "hpack.h"

#include <array>
#include <utility>

namespace prism::detail::http2
{
namespace
{
struct static_entry_t
{
  std::string_view name;
  std::string_view value;
};

constexpr std::array<static_entry_t, 61> static_table{{
  {":authority", ""},
  {":method", "GET"},
  {":method", "POST"},
  {":path", "/"},
  {":path", "/index.html"},
  {":scheme", "http"},
  {":scheme", "https"},
  {":status", "200"},
  {":status", "204"},
  {":status", "206"},
  {":status", "304"},
  {":status", "400"},
  {":status", "404"},
  {":status", "500"},
  {"accept-charset", ""},
  {"accept-encoding", "gzip, deflate"},
  {"accept-language", ""},
  {"accept-ranges", ""},
  {"accept", ""},
  {"access-control-allow-origin", ""},
  {"age", ""},
  {"allow", ""},
  {"authorization", ""},
  {"cache-control", ""},
  {"content-disposition", ""},
  {"content-encoding", ""},
  {"content-language", ""},
  {"content-length", ""},
  {"content-location", ""},
  {"content-range", ""},
  {"content-type", ""},
  {"cookie", ""},
  {"date", ""},
  {"etag", ""},
  {"expect", ""},
  {"expires", ""},
  {"from", ""},
  {"host", ""},
  {"if-match", ""},
  {"if-modified-since", ""},
  {"if-none-match", ""},
  {"if-range", ""},
  {"if-unmodified-since", ""},
  {"last-modified", ""},
  {"link", ""},
  {"location", ""},
  {"max-forwards", ""},
  {"proxy-authenticate", ""},
  {"proxy-authorization", ""},
  {"range", ""},
  {"referer", ""},
  {"refresh", ""},
  {"retry-after", ""},
  {"server", ""},
  {"set-cookie", ""},
  {"strict-transport-security", ""},
  {"transfer-encoding", ""},
  {"user-agent", ""},
  {"vary", ""},
  {"via", ""},
  {"www-authenticate", ""},
}};

constexpr std::size_t static_table_size = static_table.size();
constexpr std::size_t symbol_count = 257;
constexpr std::size_t eos_symbol = 256;
constexpr int max_code_bits = 30;

constexpr std::array<std::uint8_t, symbol_count> huffman_bits{
  13, 23, 28, 28, 28, 28, 28, 28, 28, 24, 30, 28, 28, 30, 28, 28,
  28, 28, 28, 28, 28, 28, 30, 28, 28, 28, 28, 28, 28, 28, 28, 28,
  6, 10, 10, 12, 13, 6, 8, 11, 10, 10, 8, 11, 8, 6, 6, 6,
  5, 5, 5, 6, 6, 6, 6, 6, 6, 6, 7, 8, 15, 6, 12, 10,
  13, 6, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
  7, 7, 7, 7, 7, 7, 7, 7, 8, 7, 8, 13, 19, 13, 14, 6,
  15, 5, 6, 5, 6, 5, 6, 6, 6, 5, 7, 7, 6, 6, 6, 5,
  6, 7, 6, 5, 5, 6, 7, 7, 7, 7, 7, 15, 11, 14, 13, 28,
  20, 22, 20, 20, 22, 22, 22, 23, 22, 23, 23, 23, 23, 23, 24, 23,
  24, 24, 22, 23, 24, 23, 23, 23, 23, 21, 22, 23, 22, 23, 23, 24,
  22, 21, 20, 22, 22, 23, 23, 21, 23, 22, 22, 24, 21, 22, 23, 23,
  21, 21, 22, 21, 23, 22, 23, 23, 20, 22, 22, 22, 23, 22, 22, 23,
  26, 26, 20, 19, 22, 23, 22, 25, 26, 26, 26, 27, 27, 26, 24, 25,
  19, 21, 26, 27, 27, 26, 27, 24, 21, 21, 26, 26, 28, 27, 27, 27,
  20, 24, 20, 21, 22, 21, 21, 23, 22, 22, 25, 25, 24, 24, 26, 23,
  26, 27, 26, 26, 27, 27, 27, 27, 27, 28, 27, 27, 27, 27, 27, 26,
  30,
};

struct huffman_tables_t
{
  std::array<std::uint32_t, symbol_count> code{};
  std::array<std::uint8_t, symbol_count> length{};
  std::array<std::uint32_t, max_code_bits + 1> first_code{};
  std::array<std::uint32_t, max_code_bits + 1> count{};
  std::array<int, max_code_bits + 1> first_index{};
  std::array<int, symbol_count> sorted_symbols{};
};

huffman_tables_t build_huffman_tables()
{
  huffman_tables_t t{};
  for (std::size_t s = 0; s < symbol_count; ++s)
  {
    t.count[huffman_bits[s]] += 1;
  }
  int idx = 0;
  for (int len = 1; len <= max_code_bits; ++len)
  {
    t.first_index[len] = idx;
    for (std::size_t s = 0; s < symbol_count; ++s)
    {
      if (huffman_bits[s] == len)
      {
        t.sorted_symbols[static_cast<std::size_t>(idx)] = static_cast<int>(s);
        ++idx;
      }
    }
  }
  std::uint32_t code = 0;
  for (int len = 1; len <= max_code_bits; ++len)
  {
    t.first_code[len] = code;
    for (std::uint32_t k = 0; k < t.count[len]; ++k)
    {
      int sym = t.sorted_symbols[static_cast<std::size_t>(t.first_index[len]) + k];
      t.code[static_cast<std::size_t>(sym)] = code + k;
      t.length[static_cast<std::size_t>(sym)] = static_cast<std::uint8_t>(len);
    }
    code = (code + t.count[len]) << 1;
  }
  return t;
}

const huffman_tables_t &huffman_tables()
{
  static const huffman_tables_t tables = build_huffman_tables();
  return tables;
}

int huffman_lookup(const huffman_tables_t &t, int len, std::uint32_t code)
{
  if (len < 1 || len > max_code_bits || t.count[len] == 0)
  {
    return -1;
  }
  if (code < t.first_code[len])
  {
    return -1;
  }
  std::uint32_t offset = code - t.first_code[len];
  if (offset >= t.count[len])
  {
    return -1;
  }
  return t.sorted_symbols[static_cast<std::size_t>(t.first_index[len]) + offset];
}

bool decode_integer(std::string_view block, std::size_t &pos, unsigned prefix_bits, std::uint64_t &value)
{
  if (pos >= block.size())
  {
    return false;
  }
  std::uint32_t mask = (1u << prefix_bits) - 1;
  std::uint64_t result = static_cast<unsigned char>(block[pos]) & mask;
  ++pos;
  if (result < mask)
  {
    value = result;
    return true;
  }
  unsigned shift = 0;
  for (;;)
  {
    if (pos >= block.size())
    {
      return false;
    }
    unsigned char byte = static_cast<unsigned char>(block[pos]);
    ++pos;
    result += static_cast<std::uint64_t>(byte & 0x7f) << shift;
    if (result > 0x7fffffff)
    {
      return false;
    }
    if ((byte & 0x80) == 0)
    {
      break;
    }
    shift += 7;
    if (shift > 28)
    {
      return false;
    }
  }
  value = result;
  return true;
}

void encode_integer(std::string &out, std::uint64_t value, unsigned prefix_bits, std::uint8_t flags)
{
  std::uint32_t mask = (1u << prefix_bits) - 1;
  if (value < mask)
  {
    out.push_back(static_cast<char>(flags | static_cast<std::uint8_t>(value)));
    return;
  }
  out.push_back(static_cast<char>(flags | static_cast<std::uint8_t>(mask)));
  value -= mask;
  while (value >= 128)
  {
    out.push_back(static_cast<char>((value & 0x7f) | 0x80));
    value >>= 7;
  }
  out.push_back(static_cast<char>(value));
}

bool decode_string(std::string_view block, std::size_t &pos, std::string &out)
{
  if (pos >= block.size())
  {
    return false;
  }
  bool huffman = (static_cast<unsigned char>(block[pos]) & 0x80) != 0;
  std::uint64_t length = 0;
  if (!decode_integer(block, pos, 7, length))
  {
    return false;
  }
  if (length > block.size() - pos)
  {
    return false;
  }
  std::string_view raw = block.substr(pos, length);
  pos += length;
  if (huffman)
  {
    return huffman_decode(raw, out);
  }
  out.assign(raw);
  return true;
}
} // namespace

std::string huffman_encode(std::string_view input)
{
  const huffman_tables_t &t = huffman_tables();
  std::string out;
  std::uint64_t acc = 0;
  int nbits = 0;
  for (char cc : input)
  {
    unsigned char c = static_cast<unsigned char>(cc);
    acc = (acc << t.length[c]) | t.code[c];
    nbits += t.length[c];
    while (nbits >= 8)
    {
      nbits -= 8;
      out.push_back(static_cast<char>((acc >> nbits) & 0xff));
    }
    acc &= (std::uint64_t{1} << nbits) - 1;
  }
  if (nbits > 0)
  {
    int pad = 8 - nbits;
    std::uint64_t last = (acc << pad) | ((std::uint64_t{1} << pad) - 1);
    out.push_back(static_cast<char>(last & 0xff));
  }
  return out;
}

bool huffman_decode(std::string_view input, std::string &out)
{
  const huffman_tables_t &t = huffman_tables();
  std::uint32_t code = 0;
  int len = 0;
  for (char cc : input)
  {
    unsigned char byte = static_cast<unsigned char>(cc);
    for (int bit = 7; bit >= 0; --bit)
    {
      code = (code << 1) | ((byte >> bit) & 1u);
      ++len;
      if (len > max_code_bits)
      {
        return false;
      }
      int sym = huffman_lookup(t, len, code);
      if (sym >= 0)
      {
        if (static_cast<std::size_t>(sym) == eos_symbol)
        {
          return false;
        }
        out.push_back(static_cast<char>(sym));
        code = 0;
        len = 0;
      }
    }
  }
  if (len > 7)
  {
    return false;
  }
  if (len > 0)
  {
    std::uint32_t ones = (1u << len) - 1;
    if (code != ones)
    {
      return false;
    }
  }
  return true;
}

hpack_decoder_t::hpack_decoder_t(std::uint32_t max_dynamic_size)
  : _max_dynamic(max_dynamic_size), _protocol_max_dynamic(max_dynamic_size)
{
}

void hpack_decoder_t::set_protocol_max_dynamic_size(std::uint32_t size)
{
  _protocol_max_dynamic = size;
  if (_max_dynamic > size)
  {
    apply_size_update(size);
  }
}

void hpack_decoder_t::set_max_list_bytes(std::size_t bytes)
{
  _max_list_bytes = bytes;
}

std::size_t hpack_decoder_t::dynamic_size() const
{
  return _dynamic_bytes;
}

std::size_t hpack_decoder_t::dynamic_count() const
{
  return _dynamic.size();
}

bool hpack_decoder_t::table_lookup(std::uint64_t index, hpack_header_t &out) const
{
  if (index == 0)
  {
    return false;
  }
  if (index <= static_table_size)
  {
    const static_entry_t &entry = static_table[static_cast<std::size_t>(index - 1)];
    out.name.assign(entry.name);
    out.value.assign(entry.value);
    return true;
  }
  std::uint64_t di = index - static_table_size - 1;
  if (di >= _dynamic.size())
  {
    return false;
  }
  out = _dynamic[static_cast<std::size_t>(di)];
  return true;
}

void hpack_decoder_t::dynamic_insert(hpack_header_t entry)
{
  std::size_t entry_size = entry.name.size() + entry.value.size() + 32;
  while (!_dynamic.empty() && _dynamic_bytes + entry_size > _max_dynamic)
  {
    const hpack_header_t &back = _dynamic.back();
    _dynamic_bytes -= back.name.size() + back.value.size() + 32;
    _dynamic.pop_back();
  }
  if (entry_size > _max_dynamic)
  {
    return;
  }
  _dynamic_bytes += entry_size;
  _dynamic.push_front(std::move(entry));
}

void hpack_decoder_t::apply_size_update(std::uint32_t new_max)
{
  _max_dynamic = new_max;
  while (!_dynamic.empty() && _dynamic_bytes > _max_dynamic)
  {
    const hpack_header_t &back = _dynamic.back();
    _dynamic_bytes -= back.name.size() + back.value.size() + 32;
    _dynamic.pop_back();
  }
}

bool hpack_decoder_t::decode(std::string_view block, std::vector<hpack_header_t> &out)
{
  std::size_t pos = 0;
  std::size_t list_bytes = 0;
  bool field_seen = false;
  while (pos < block.size())
  {
    unsigned char first = static_cast<unsigned char>(block[pos]);
    if ((first & 0x80) != 0)
    {
      std::uint64_t index = 0;
      if (!decode_integer(block, pos, 7, index))
      {
        return false;
      }
      hpack_header_t header;
      if (!table_lookup(index, header))
      {
        return false;
      }
      list_bytes += header.name.size() + header.value.size() + 32;
      if (list_bytes > _max_list_bytes)
      {
        return false;
      }
      field_seen = true;
      out.push_back(std::move(header));
    }
    else if ((first & 0x40) != 0)
    {
      std::uint64_t name_index = 0;
      if (!decode_integer(block, pos, 6, name_index))
      {
        return false;
      }
      hpack_header_t header;
      if (name_index == 0)
      {
        if (!decode_string(block, pos, header.name))
        {
          return false;
        }
      }
      else
      {
        hpack_header_t named;
        if (!table_lookup(name_index, named))
        {
          return false;
        }
        header.name = std::move(named.name);
      }
      if (!decode_string(block, pos, header.value))
      {
        return false;
      }
      list_bytes += header.name.size() + header.value.size() + 32;
      if (list_bytes > _max_list_bytes)
      {
        return false;
      }
      field_seen = true;
      dynamic_insert(header);
      out.push_back(std::move(header));
    }
    else if ((first & 0x20) != 0)
    {
      if (field_seen)
      {
        return false;
      }
      std::uint64_t new_max = 0;
      if (!decode_integer(block, pos, 5, new_max))
      {
        return false;
      }
      if (new_max > _protocol_max_dynamic)
      {
        return false;
      }
      apply_size_update(static_cast<std::uint32_t>(new_max));
    }
    else
    {
      std::uint64_t name_index = 0;
      if (!decode_integer(block, pos, 4, name_index))
      {
        return false;
      }
      hpack_header_t header;
      if (name_index == 0)
      {
        if (!decode_string(block, pos, header.name))
        {
          return false;
        }
      }
      else
      {
        hpack_header_t named;
        if (!table_lookup(name_index, named))
        {
          return false;
        }
        header.name = std::move(named.name);
      }
      if (!decode_string(block, pos, header.value))
      {
        return false;
      }
      list_bytes += header.name.size() + header.value.size() + 32;
      if (list_bytes > _max_list_bytes)
      {
        return false;
      }
      field_seen = true;
      out.push_back(std::move(header));
    }
  }
  return true;
}

hpack_encoder_t::hpack_encoder_t(bool use_huffman)
  : _use_huffman(use_huffman)
{
}

namespace
{
int static_full_match(std::string_view name, std::string_view value)
{
  for (std::size_t i = 0; i < static_table_size; ++i)
  {
    if (static_table[i].name == name && static_table[i].value == value)
    {
      return static_cast<int>(i + 1);
    }
  }
  return -1;
}

int static_name_match(std::string_view name)
{
  for (std::size_t i = 0; i < static_table_size; ++i)
  {
    if (static_table[i].name == name)
    {
      return static_cast<int>(i + 1);
    }
  }
  return -1;
}

void encode_string(std::string &out, std::string_view value, bool use_huffman)
{
  if (use_huffman)
  {
    std::string encoded = huffman_encode(value);
    if (encoded.size() < value.size())
    {
      encode_integer(out, encoded.size(), 7, 0x80);
      out.append(encoded);
      return;
    }
  }
  encode_integer(out, value.size(), 7, 0);
  out.append(value);
}
} // namespace

std::string hpack_encoder_t::encode(const std::vector<hpack_header_t> &headers) const
{
  std::string out;
  for (const hpack_header_t &header : headers)
  {
    int full = static_full_match(header.name, header.value);
    if (full >= 0)
    {
      encode_integer(out, static_cast<std::uint64_t>(full), 7, 0x80);
      continue;
    }
    int name_index = static_name_match(header.name);
    if (name_index >= 0)
    {
      encode_integer(out, static_cast<std::uint64_t>(name_index), 4, 0);
    }
    else
    {
      out.push_back(0);
      encode_string(out, header.name, _use_huffman);
    }
    encode_string(out, header.value, _use_huffman);
  }
  return out;
}
} // namespace prism::detail::http2
