#pragma once

#include <memory>

#include <vio/operation/tcp_server.h>
#include <vio/task.h>

#include "../error.h"
#include "../router.h"
#include "../server_options.h"

namespace vio
{
class cancellation_t;
}

namespace prism::detail
{
vio::task_t<void> serve_connection(vio::tcp_t client, std::shared_ptr<const router_t> router, keepalive_options_t options);

vio::task_t<result_t<void>> serve(vio::tcp_server_t server, std::shared_ptr<const router_t> router, vio::cancellation_t *cancel, keepalive_options_t options);
} // namespace prism::detail
