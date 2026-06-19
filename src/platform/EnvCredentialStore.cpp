#include "platform/EnvCredentialStore.h"

#include <cstdlib>

namespace pbr {

std::string EnvCredentialStore::Get(const std::string& key) const {
  if (const char* value = std::getenv(key.c_str())) {
    return value;
  }
  return {};
}

} // namespace pbr
