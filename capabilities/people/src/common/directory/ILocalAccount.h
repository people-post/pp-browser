#pragma once

#include "common/Error.h"
#include "common/PbrCompat.h"

#include <cstdint>
#include <string>

namespace pbr {

/**
 * This device's account as a feature that speaks for it reads it: who it is, and what a stranger's
 * call must offer — never its keys.
 */
class ILocalAccount {
public:
  virtual ~ILocalAccount() = default;

  virtual Roe<std::string> GetAccountId() const = 0;
  /** The least (minor units) an invite from someone not yet open with us must offer (P001); 0: free. */
  virtual Roe<int64_t> InitiationFloor() const = 0;
};

} // namespace pbr
