#include "prism/logging.h"

#include <cstdio>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>

namespace prism
{
namespace
{
std::string log_timestamp()
{
  std::time_t now = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &now);
#else
  gmtime_r(&now, &tm);
#endif
  char buf[32];
  std::size_t n = std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return std::string(buf, n);
}
} // namespace

log_sink_t default_stdout_sink()
{
  return [mutex = std::make_shared<std::mutex>()](log_level_t level, std::string_view message)
  {
    std::FILE *stream = (level == log_level_t::warn || level == log_level_t::error) ? stderr : stdout;
    std::string_view name = log_level_name(level);
    std::string stamp = log_timestamp();
    std::lock_guard<std::mutex> guard(*mutex);
    std::fprintf(stream, "%s [%.*s] %.*s\n", stamp.c_str(), static_cast<int>(name.size()), name.data(), static_cast<int>(message.size()), message.data());
    std::fflush(stream);
  };
}

logger_t::logger_t()
  : _sink(default_stdout_sink())
{
}
} // namespace prism
