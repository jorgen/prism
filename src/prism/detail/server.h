#pragma once

#include <memory>

#include <vio/operation/tcp_server.h>
#include <vio/operation/tls_server.h>
#include <vio/task.h>

#include "../error.h"
#include "../logging.h"
#include "../router.h"
#include "../server_options.h"

namespace vio
{
class cancellation_t;
}

namespace prism::detail
{
vio::task_t<void> serve_connection(vio::tcp_t client, std::shared_ptr<const router_t> router, std::shared_ptr<const logger_t> logger, server_options_t options);

vio::task_t<result_t<void>> serve(vio::tcp_server_t server, std::shared_ptr<const router_t> router, std::shared_ptr<const logger_t> logger, vio::cancellation_t *cancel, server_options_t options);

vio::task_t<result_t<void>> serve_tls(vio::ssl_server_t server, std::shared_ptr<const router_t> router, std::shared_ptr<const logger_t> logger, vio::cancellation_t *cancel, server_options_t options);
} // namespace prism::detail
