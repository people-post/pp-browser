#pragma once

#include "common/Error.h"
#include "common/PbrCompat.h"

#include <string>
#include <vector>

namespace pbr {

using AccountSigningSecret = std::vector<uint8_t>;

/** Unlocked account ML-DSA signing material for durable local receipts (P002). */
class IAccountSigningAccess {
public:
  virtual ~IAccountSigningAccess() = default;

  virtual Roe<std::string> GetAccountId() const = 0;
  virtual Roe<AccountSigningSecret> GetAccountMlDsaPrivateKey() const = 0;
};

} // namespace pbr
