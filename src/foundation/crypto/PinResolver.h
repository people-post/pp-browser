#pragma once

#include "common/Error.h"

#include <string>
#include <string_view>
#include "common/PbrCompat.h"

namespace pbr {

/** Resolve profile PIN from CLI flag, then PP_BROWSER_PIN env. */
class PinResolver {
public:
  static Roe<std::string> Resolve(std::string_view cli_pin);
  static Roe<std::string> Require(std::string_view cli_pin);
};

} // namespace pbr
