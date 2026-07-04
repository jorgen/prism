#include "prism/content.h"

#include <array>
#include <cctype>
#include <cstddef>

namespace prism
{
namespace
{
bool iequals(std::string_view a, std::string_view b)
{
  if (a.size() != b.size())
  {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i)
  {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
    {
      return false;
    }
  }
  return true;
}

std::string_view trim(std::string_view in)
{
  while (!in.empty() && (in.front() == ' ' || in.front() == '\t'))
  {
    in.remove_prefix(1);
  }
  while (!in.empty() && (in.back() == ' ' || in.back() == '\t'))
  {
    in.remove_suffix(1);
  }
  return in;
}

struct media_row_t
{
  std::string_view media;
  format_t format;
};

constexpr std::array<media_row_t, 8> media_rows{{
  {"application/json", format_t::json},
  {"text/json", format_t::json},
  {"application/yaml", format_t::yaml},
  {"application/x-yaml", format_t::yaml},
  {"text/yaml", format_t::yaml},
  {"text/x-yaml", format_t::yaml},
  {"application/cbor", format_t::cbor},
  {"application/x-cbor", format_t::cbor},
}};

constexpr std::array<format_t, 3> preference_order{format_t::json, format_t::yaml, format_t::cbor};

std::string_view type_of(std::string_view media)
{
  std::size_t slash = media.find('/');
  return slash == std::string_view::npos ? media : media.substr(0, slash);
}

int match_specificity(std::string_view range, format_t format)
{
  if (range == "*/*" || range == "*")
  {
    return 0;
  }
  if (range.size() >= 2 && range.substr(range.size() - 2) == "/*")
  {
    std::string_view range_type = range.substr(0, range.size() - 2);
    for (const auto &row : media_rows)
    {
      if (row.format == format && iequals(type_of(row.media), range_type))
      {
        return 1;
      }
    }
    return -1;
  }
  for (const auto &row : media_rows)
  {
    if (row.format == format && iequals(row.media, range))
    {
      return 2;
    }
  }
  return -1;
}

double parse_q_value(std::string_view value)
{
  value = trim(value);
  bool any = false;
  double result = 0.0;
  std::size_t i = 0;
  while (i < value.size() && value[i] >= '0' && value[i] <= '9')
  {
    result = result * 10.0 + (value[i] - '0');
    ++i;
    any = true;
  }
  if (i < value.size() && value[i] == '.')
  {
    ++i;
    double scale = 0.1;
    while (i < value.size() && value[i] >= '0' && value[i] <= '9')
    {
      result += (value[i] - '0') * scale;
      scale *= 0.1;
      ++i;
      any = true;
    }
  }
  return any ? result : 1.0;
}

double quality_of(std::string_view params)
{
  double q = 1.0;
  while (!params.empty())
  {
    std::size_t semi = params.find(';');
    std::string_view token = trim(semi == std::string_view::npos ? params : params.substr(0, semi));
    if (token.size() >= 2 && (token[0] == 'q' || token[0] == 'Q') && token[1] == '=')
    {
      q = parse_q_value(token.substr(2));
    }
    if (semi == std::string_view::npos)
    {
      break;
    }
    params = params.substr(semi + 1);
  }
  return q;
}
} // namespace

std::string_view media_type_of(format_t format)
{
  switch (format)
  {
  case format_t::yaml:
    return "application/yaml";
  case format_t::cbor:
    return "application/cbor";
  case format_t::json:
    return "application/json";
  }
  return "application/json";
}

std::optional<format_t> format_for_content_type(std::string_view content_type)
{
  std::size_t semi = content_type.find(';');
  std::string_view media = trim(semi == std::string_view::npos ? content_type : content_type.substr(0, semi));
  for (const auto &row : media_rows)
  {
    if (iequals(row.media, media))
    {
      return row.format;
    }
  }
  return std::nullopt;
}

struct candidate_t
{
  format_t format;
  int spec = -1;
  double q = 0.0;
};

std::optional<format_t> negotiate_accept(std::string_view accept_header)
{
  std::array<candidate_t, 3> candidates{{{format_t::json}, {format_t::yaml}, {format_t::cbor}}};
  bool any_entry = false;

  std::string_view rest = accept_header;
  while (!rest.empty())
  {
    std::size_t comma = rest.find(',');
    std::string_view entry = trim(comma == std::string_view::npos ? rest : rest.substr(0, comma));
    if (!entry.empty())
    {
      any_entry = true;
      std::size_t semi = entry.find(';');
      std::string_view range = trim(semi == std::string_view::npos ? entry : entry.substr(0, semi));
      double q = semi == std::string_view::npos ? 1.0 : quality_of(entry.substr(semi + 1));
      for (candidate_t &candidate : candidates)
      {
        int spec = match_specificity(range, candidate.format);
        if (spec < 0)
        {
          continue;
        }
        if (spec > candidate.spec || (spec == candidate.spec && q > candidate.q))
        {
          candidate.spec = spec;
          candidate.q = q;
        }
      }
    }
    if (comma == std::string_view::npos)
    {
      break;
    }
    rest = rest.substr(comma + 1);
  }

  if (!any_entry)
  {
    return format_t::json;
  }

  std::optional<format_t> chosen;
  double chosen_q = 0.0;
  for (format_t format : preference_order)
  {
    for (const candidate_t &candidate : candidates)
    {
      if (candidate.format == format && candidate.spec >= 0 && candidate.q > 0.0 && (!chosen || candidate.q > chosen_q))
      {
        chosen = format;
        chosen_q = candidate.q;
      }
    }
  }
  return chosen;
}
} // namespace prism
