#include <doctest/doctest.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <prism/logging.h>

TEST_CASE("logger defaults to info level with a sink")
{
  prism::logger_t logger;
  CHECK(logger.level() == prism::log_level_t::info);
  CHECK(logger.enabled(prism::log_level_t::info));
  CHECK(logger.enabled(prism::log_level_t::error));
  CHECK_FALSE(logger.enabled(prism::log_level_t::debug));
  CHECK_FALSE(logger.enabled(prism::log_level_t::trace));
}

TEST_CASE("logger routes only messages at or above its level to the sink")
{
  std::vector<std::pair<prism::log_level_t, std::string>> captured;
  prism::logger_t logger;
  logger.set_level(prism::log_level_t::warn);
  logger.set_sink([&captured](prism::log_level_t level, std::string_view message) { captured.emplace_back(level, std::string(message)); });

  logger.log(prism::log_level_t::debug, "d");
  logger.log(prism::log_level_t::info, "i");
  logger.log(prism::log_level_t::warn, "w");
  logger.log(prism::log_level_t::error, "e");

  REQUIRE(captured.size() == 2);
  CHECK(captured[0].first == prism::log_level_t::warn);
  CHECK(captured[0].second == "w");
  CHECK(captured[1].first == prism::log_level_t::error);
  CHECK(captured[1].second == "e");
}

TEST_CASE("logger at off level emits nothing")
{
  int count = 0;
  prism::logger_t logger;
  logger.set_level(prism::log_level_t::off);
  logger.set_sink([&count](prism::log_level_t, std::string_view) { ++count; });
  logger.log(prism::log_level_t::error, "e");
  CHECK_FALSE(logger.enabled(prism::log_level_t::error));
  CHECK(count == 0);
}

TEST_CASE("logger with a cleared sink is disabled")
{
  prism::logger_t logger;
  logger.set_sink({});
  CHECK_FALSE(logger.enabled(prism::log_level_t::error));
}

TEST_CASE("log_level_name covers every level")
{
  CHECK(prism::log_level_name(prism::log_level_t::trace) == "TRACE");
  CHECK(prism::log_level_name(prism::log_level_t::debug) == "DEBUG");
  CHECK(prism::log_level_name(prism::log_level_t::info) == "INFO");
  CHECK(prism::log_level_name(prism::log_level_t::warn) == "WARN");
  CHECK(prism::log_level_name(prism::log_level_t::error) == "ERROR");
  CHECK(prism::log_level_name(prism::log_level_t::off) == "OFF");
}
