#include "base/crypto/PinResolver.h"

#include <cstdlib>

namespace pbr {

Roe<std::string> PinResolver::Resolve(std::string_view cli_pin) {
  if (!cli_pin.empty()) {
    return std::string(cli_pin);
  }
  const char* env = std::getenv("PP_BROWSER_PIN");
  if (env != nullptr && env[0] != '\0') {
    return std::string(env);
  }
  return Error("Profile PIN required: pass --pin or set PP_BROWSER_PIN");
}

Roe<std::string> PinResolver::Require(std::string_view cli_pin) {
  return Resolve(cli_pin);
}

} // namespace pbr
