#pragma once

#include <expected>
#include <string>
#include <utility>

#include "status.h"

namespace prism
{
// An error carries an HTTP status and a human-readable message. The shape
// mirrors vio::error_t (a code + a msg) so the two libraries feel consistent.
struct error_t
{
  status_t code = status_t::internal_server_error;
  std::string msg;
};

// prism::result_t<T> is std::expected<T, error_t>, matching vio's use of
// std::expected<T, error_t>. Handlers and helpers return this so failures
// carry a status code straight through to the response.
template <typename T>
using result_t = std::expected<T, error_t>;

// Convenience for constructing an unexpected error.
inline std::unexpected<error_t> fail(status_t code, std::string msg)
{
  return std::unexpected(error_t{code, std::move(msg)});
}
} // namespace prism
