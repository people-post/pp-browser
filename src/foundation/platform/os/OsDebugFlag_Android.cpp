#include "foundation/platform/os/OsDebugFlag.h"

#include <sys/system_properties.h>

#include <cstring>

namespace pbr::os {

bool DebugFlagSet(const char* name) {
  char value[PROP_VALUE_MAX] = {};
  return name != nullptr && __system_property_get(name, value) > 0 && std::strcmp(value, "1") == 0;
}

} // namespace pbr::os
