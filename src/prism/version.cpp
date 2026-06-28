#include "prism/prism.h"

#ifndef PRISM_VERSION
#define PRISM_VERSION "0.0.0"
#endif

namespace prism
{
const char *version()
{
  return PRISM_VERSION;
}
} // namespace prism
