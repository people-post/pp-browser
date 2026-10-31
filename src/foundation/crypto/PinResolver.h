#pragma once

#include "common/Secret.h"

#include "common/Error.h"

#include <string>
#include <string_view>
#include "common/PbrCompat.h"

namespace pbr {

/** Resolve profile PIN from CLI flag, then PP_BROWSER_PIN env. */
class PinResolver {
public:
  static Roe<SecretText> Resolve(std::string_view cli_pin);
  static Roe<SecretText> Require(std::string_view cli_pin);
};

} // namespace pbr
