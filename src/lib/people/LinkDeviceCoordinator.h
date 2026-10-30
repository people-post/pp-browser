#pragma once

#include "foundation/crypto/DataKeyVault.h"
#include "foundation/crypto/ProfileSecretsEngine.h"
#include "lib/people/IdentityStore.h"
#include "lib/people/LinkDeviceExchange.h"

#include "common/Error.h"

#include <string>
#include <string_view>
#include "common/PbrCompat.h"

namespace pbr {

/** Export / import `pp-browser-link-device-v1` (M012) over the profile's identity and vault. */
class LinkDeviceCoordinator {
public:
  /**
   * A bundle for a new device (M021): the account and this device's account card, which the new device signs
   * itself into. No sessions ride: the new device opens its own. `ttl_ms`: 15 min for a pasted transfer code.
   */
  static Roe<std::string> ExportJson(IdentityStore& identity, DataKeyVault& vault, std::string account_card_json,
                                     int64_t now_ms, int64_t ttl_ms = kLinkDeviceDefaultTtlMs);

  /** Vault + account import, then the DEK fan-out (`secrets` may be null in tests). */
  static Roe<LinkDeviceImportResult> Import(IdentityStore& identity, DataKeyVault& vault,
                                            ProfileSecretsEngine* secrets, const std::string& bundle_json,
                                            std::string_view pin, int64_t now_ms);
};

} // namespace pbr
