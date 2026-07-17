#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "app.h"
#include "http.h"
#include "router.h"

namespace prism
{
struct backend_t
{
  std::string host;
  std::uint16_t port = 80;
};

class reverse_proxy_t
{
public:
  void add_route(std::string host, backend_t backend);
  void install(app_t &app) const;
  [[nodiscard]] handler_t handler() const;
  [[nodiscard]] ws_handler_t ws_handler() const;

private:
  std::shared_ptr<std::vector<std::pair<std::string, backend_t>>> _table = std::make_shared<std::vector<std::pair<std::string, backend_t>>>();
};
} // namespace prism
