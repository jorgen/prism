#pragma once

#include <string>
#include <string_view>

#include "router.h"

namespace prism
{
// Build a handler that serves files from `root`. Register it on a wildcard route
// ("<prefix>/{path...}") — the captured tail names the file relative to `root`.
// Path traversal ("..") is rejected; an empty tail or a directory serves
// `index`. Files are read asynchronously (vio) and returned whole. Unknown
// extensions fall back to application/octet-stream; a missing file is 404.
[[nodiscard]] handler_t static_file_handler(std::string root, std::string index = "index.html");

// Guess a Content-Type from a path's extension (application/octet-stream if none).
[[nodiscard]] std::string_view content_type_for_path(std::string_view path);
} // namespace prism
