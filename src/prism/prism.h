#pragma once

// prism — a lean REST service library built on vio (async I/O) and structify
// (JSON <-> struct). Include this single header to pull in the whole API.

#include <prism/prism_export.h>

#include <prism/app.h>
#include <prism/error.h>
#include <prism/http.h>
#include <prism/json.h>
#include <prism/logging.h>
#include <prism/router.h>
#include <prism/server_options.h>
#include <prism/status.h>

namespace prism
{
// Library version string, e.g. "0.0.1".
PRISM_EXPORT const char *version();
} // namespace prism
