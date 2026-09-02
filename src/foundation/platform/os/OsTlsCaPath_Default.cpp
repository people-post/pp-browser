#if !defined(__ANDROID__)

#include "foundation/platform/os/OsTlsCaPath.h"

namespace pbr::os {

const char* TlsCaPath() {
  return nullptr;
}

} // namespace pbr::os

#endif
