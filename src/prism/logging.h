#pragma once

#include <cstdint>
#include <functional>
#include <string_view>
#include <utility>

namespace prism
{
enum class log_level_t : uint8_t
{
  trace,
  debug,
  info,
  warn,
  error,
  off,
};

constexpr std::string_view log_level_name(log_level_t level)
{
  switch (level)
  {
  case log_level_t::trace:
    return "TRACE";
  case log_level_t::debug:
    return "DEBUG";
  case log_level_t::info:
    return "INFO";
  case log_level_t::warn:
    return "WARN";
  case log_level_t::error:
    return "ERROR";
  case log_level_t::off:
    return "OFF";
  }
  return "OFF";
}

using log_sink_t = std::function<void(log_level_t, std::string_view)>;

log_sink_t default_stdout_sink();

class logger_t
{
public:
  logger_t();

  void set_sink(log_sink_t sink)
  {
    _sink = std::move(sink);
  }
  void set_level(log_level_t level)
  {
    _level = level;
  }
  [[nodiscard]] log_level_t level() const
  {
    return _level;
  }
  [[nodiscard]] bool enabled(log_level_t level) const
  {
    return static_cast<bool>(_sink) && level != log_level_t::off && level >= _level;
  }
  void log(log_level_t level, std::string_view message) const
  {
    if (enabled(level))
    {
      _sink(level, message);
    }
  }

private:
  log_level_t _level = log_level_t::info;
  log_sink_t _sink;
};
} // namespace prism
