#pragma once

#include "common/Error.h"
#include "common/PbrCompat.h"

#include <string>

namespace pbr {

/** Narrow contacts port for payment-promise avoid / trust checks (P002). */
class IContactTrustAccess {
public:
  virtual ~IContactTrustAccess() = default;

  /** True when a contact row exists for account_id and trust is Blocked. */
  virtual Roe<bool> IsAccountBlocked(const std::string& account_id) const = 0;

  /** Best-effort Blocked stamp when a matching Account ID contact exists. */
  virtual Roe<void> BlockAccountIfPresent(const std::string& account_id) = 0;
};

} // namespace pbr
