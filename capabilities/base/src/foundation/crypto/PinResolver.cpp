#include "foundation/crypto/PinResolver.h"

#include "foundation/error/AppError.h"

#include <cstdlib>
#include "common/PbrCompat.h"

namespace pbr {

Roe<SecretText> PinResolver::Resolve(std::string_view cli_pin) {
  if (!cli_pin.empty()) {
    return SecretText(cli_pin);
  }
  const char* env = std::getenv("PP_BROWSER_PIN");
  if (env != nullptr && env[0] != '\0') {
    return SecretText(env);
  }
  return AppError::Pin(Err::Pin::Required, "No PIN provided");
}

Roe<SecretText> PinResolver::Require(std::string_view cli_pin) {
  auto resolved = Resolve(cli_pin);
  if (!resolved) {
    return AppError::Pin(Err::Pin::Required, "Profile PIN required via --pin, PP_BROWSER_PIN, or in-app unlock");
  }
  return resolved;
}

} // namespace pbr
