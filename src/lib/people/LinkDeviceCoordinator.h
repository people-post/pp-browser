#pragma once

#include "foundation/crypto/DataKeyVault.h"
#include "foundation/crypto/IPskSessionStore.h"
#include "foundation/crypto/LinkDeviceCodec.h"
#include "foundation/crypto/ProfileSecretsEngine.h"
#include "lib/people/IdentityStore.h"
#include "lib/people/LinkDeviceExchange.h"

#include "common/Error.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** Export / import `pp-browser-link-device-v1` plus public PSK apply (M012 / M014). */
class LinkDeviceCoordinator {
public:
  static Roe<std::vector<LinkDevicePublicPsk>> CollectPublicPsks(IPskSessionStore& psk_store);
  static Roe<void> ApplyPublicPsks(IPskSessionStore& psk_store, const std::vector<LinkDevicePublicPsk>& rows);

  /** `ttl_ms`: 15 min for a pasted transfer code; the local account backup passes a very long one. */
  static Roe<std::string> ExportJson(IdentityStore& identity, DataKeyVault& vault, IPskSessionStore& psk_store,
                                     int64_t now_ms, int64_t ttl_ms = kLinkDeviceDefaultTtlMs,
                                     std::optional<std::string> account_card_json = std::nullopt);

  /**
   * Vault + account import, then optional DEK fan-out, then public PSK apply.
   * `secrets` may be null in tests; caller must SetDek on the PSK store first.
   */
  static Roe<LinkDeviceImportResult> Import(IdentityStore& identity, DataKeyVault& vault, IPskSessionStore& psk_store,
                                            ProfileSecretsEngine* secrets, const std::string& bundle_json,
                                            std::string_view pin, int64_t now_ms);
};

} // namespace pbr
