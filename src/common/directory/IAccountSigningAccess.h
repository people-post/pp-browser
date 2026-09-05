#pragma once

#include "common/Error.h"
#include "common/PbrCompat.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pbr {

using AccountSignBytes = std::vector<uint8_t>;

/** Unlocked account ML-DSA signing for durable local receipts (P002). */
class IAccountSigningAccess {
public:
  virtual ~IAccountSigningAccess() = default;

  virtual Roe<std::string> GetAccountId() const = 0;
  virtual Roe<std::string> SignAccountBytes(const AccountSignBytes& sign_bytes) const = 0;
};

} // namespace pbr
