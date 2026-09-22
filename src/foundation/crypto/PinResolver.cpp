#include "foundation/crypto/PinResolver.h"

#include "foundation/error/AppError.h"

#include <cstdlib>
#include "common/PbrCompat.h"

namespace pbr {

Roe<std::string> PinResolver::Resolve(std::string_view cli_pin) {
  if (!cli_pin.empty()) {
    return std::string(cli_pin);
  }
  const char* env = std::getenv("PP_BROWSER_PIN");
  if (env != nullptr && env[0] != '\0') {
    return std::string(env);
  }
#ifdef PP_BROWSER_DEV_PIN
  // Local dev builds only (-DPP_BROWSER_DEV_PIN=… / GCC_PREPROCESSOR_DEFINITIONS): skip the
  // unlock prompt on every launch, including iOS home-screen launches with no environment.
  if (PP_BROWSER_DEV_PIN[0] != '\0') {
    return std::string(PP_BROWSER_DEV_PIN);
  }
#endif
  return AppError::Pin(Err::Pin::Required, "No PIN provided");
}

Roe<std::string> PinResolver::Require(std::string_view cli_pin) {
  auto resolved = Resolve(cli_pin);
  if (!resolved) {
    return AppError::Pin(Err::Pin::Required, "Profile PIN required via --pin, PP_BROWSER_PIN, or in-app unlock");
  }
  return resolved;
}

} // namespace pbr
