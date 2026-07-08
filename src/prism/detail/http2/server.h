#pragma once

#include <memory>

#include <vio/operation/tcp.h>
#include <vio/operation/tls_server.h>
#include <vio/task.h>

#include "../../logging.h"
#include "../../router.h"
#include "../../server_options.h"

namespace prism::detail::http2
{
vio::task_t<void> serve_connection_h2(vio::tcp_t client, std::shared_ptr<const router_t> router, std::shared_ptr<const logger_t> logger, server_options_t options);

vio::task_t<void> serve_connection_h2_tls(vio::ssl_server_client_t client, std::shared_ptr<const router_t> router, std::shared_ptr<const logger_t> logger, server_options_t options);
} // namespace prism::detail::http2
