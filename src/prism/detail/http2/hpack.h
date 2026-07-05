#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace prism::detail::http2
{
struct hpack_header_t
{
  std::string name;
  std::string value;
};

class hpack_decoder_t
{
public:
  explicit hpack_decoder_t(std::uint32_t max_dynamic_size = 4096);

  bool decode(std::string_view block, std::vector<hpack_header_t> &out);

  void set_protocol_max_dynamic_size(std::uint32_t size);
  void set_max_list_bytes(std::size_t bytes);
  [[nodiscard]] std::size_t dynamic_size() const;
  [[nodiscard]] std::size_t dynamic_count() const;

private:
  bool table_lookup(std::uint64_t index, hpack_header_t &out) const;
  void dynamic_insert(hpack_header_t entry);
  void apply_size_update(std::uint32_t new_max);

  std::deque<hpack_header_t> _dynamic;
  std::size_t _dynamic_bytes = 0;
  std::uint32_t _max_dynamic = 4096;
  std::uint32_t _protocol_max_dynamic = 4096;
  std::size_t _max_list_bytes = 65536;
};

class hpack_encoder_t
{
public:
  explicit hpack_encoder_t(bool use_huffman = true);

  std::string encode(const std::vector<hpack_header_t> &headers) const;

private:
  bool _use_huffman = true;
};

std::string huffman_encode(std::string_view input);
bool huffman_decode(std::string_view input, std::string &out);
} // namespace prism::detail::http2
