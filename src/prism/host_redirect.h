#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "router.h"

namespace prism
{
// Host-level permanent redirects: a request whose Host header matches a registered from-host is
// answered with a 308 to the same path and query on the target host; everything else falls through
// to the wrapped handler. The typical use is keeping a renamed service's old hostname alive at the
// edge (308 preserves the method and body semantics, unlike 301). Host matching is case-insensitive
// and ignores any :port suffix, mirroring reverse_proxy_t.
class host_redirect_t
{
public:
  // scheme of the redirect target; the default fits a TLS-terminating edge.
  explicit host_redirect_t(std::string scheme = "https");

  void add(std::string from_host, std::string to_host);
  [[nodiscard]] bool empty() const;

  // Wrap a handler: matching hosts are redirected, the rest pass through to `inner`.
  [[nodiscard]] handler_t wrap(handler_t inner) const;

private:
  std::string _scheme;
  std::shared_ptr<std::vector<std::pair<std::string, std::string>>> _table = std::make_shared<std::vector<std::pair<std::string, std::string>>>();
};
} // namespace prism
