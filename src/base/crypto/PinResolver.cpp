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
  return Error("No PIN provided");
}

Roe<std::string> PinResolver::Require(std::string_view cli_pin) {
  auto resolved = Resolve(cli_pin);
  if (!resolved) {
    return Error("Profile PIN required: pass --pin, set PP_BROWSER_PIN, or unlock in the app");
  }
  return resolved;
}

} // namespace pbr
