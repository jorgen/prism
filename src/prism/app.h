#pragma once

#include <cstdint>
#include <exception> // vio/task.h uses std::terminate without including this
#include <string_view>
#include <utility>

#include <vio/task.h>

#include "error.h"
#include "router.h"
#include "server_options.h"

namespace vio
{
class event_loop_t;
class cancellation_t;
}

namespace prism
{
// app_t is the front door: register routes, then listen. It is a thin facade
// over router_t plus the (forthcoming) HTTP/1.1 server that drives requests
// through it on a vio event loop.
class app_t
{
public:
  void get(std::string_view pattern, handler_t handler)
  {
    _router.get(pattern, std::move(handler));
  }
  void post(std::string_view pattern, handler_t handler)
  {
    _router.post(pattern, std::move(handler));
  }
  void put(std::string_view pattern, handler_t handler)
  {
    _router.put(pattern, std::move(handler));
  }
  void patch(std::string_view pattern, handler_t handler)
  {
    _router.patch(pattern, std::move(handler));
  }
  void del(std::string_view pattern, handler_t handler)
  {
    _router.del(pattern, std::move(handler));
  }

  [[nodiscard]] const router_t &router() const
  {
    return _router;
  }

  // Run a single request through the router. Useful for tests and for
  // embedding prism behind another transport.
  [[nodiscard]] vio::task_t<response_t> handle(request_t request) const
  {
    return _router.dispatch(std::move(request));
  }

  [[nodiscard]] vio::task_t<result_t<void>> listen(vio::event_loop_t &loop, std::string_view host, uint16_t port, vio::cancellation_t *cancel = nullptr, keepalive_options_t options = {});

private:
  router_t _router;
};
} // namespace prism
