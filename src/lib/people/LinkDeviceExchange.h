#pragma once

#include "common/Secret.h"

#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/DataKeyVault.h"
#include "foundation/crypto/LinkDeviceCodec.h"
#include "lib/people/IdentityStore.h"

#include "common/Error.h"

#include <optional>
#include <string>
#include <string_view>
#include "common/PbrCompat.h"

namespace pbr {

struct LinkDeviceImportResult {
  LocalIdentity identity;
  /** The issuing device's account card, when the bundle carries one (M021; a recovery bundle does not). */
  std::optional<std::string> account_card_json;
};

/** Capture / apply `pp-browser-link-device-v1` (M012). UI and push re-attach stay in feature. */
class LinkDeviceExchange {
public:
  /** The account part of a bundle (keys, DEK, relay binding, nickname, recovery seed); no account card. */
  static Roe<LinkDeviceBundleV1> Capture(const LocalIdentity& identity, const ByteVector& dek, int64_t now_ms,
                                         int64_t ttl_ms = kLinkDeviceDefaultTtlMs);
  /** A bundle for a new device: the account and this device's account card, which it signs itself into (M021). */
  static Roe<std::string> ExportJson(const LocalIdentity& identity, const ByteVector& dek,
                                     std::string account_card_json, int64_t now_ms,
                                     int64_t ttl_ms = kLinkDeviceDefaultTtlMs);

  /**
   * Apply `pp-browser-link-device-v1`: wrap the shared DEK, keep local device
   * Ed25519 / Peer ID, replace account ML-DSA + Account ID + account KEM + relay
   * binding. Product path is an empty vault (`CreateWithDek`). Existing-vault
   * `ReplaceWithDek` remains for tests / engine; identity must already be loaded
   * under the current DEK.
   */
  static Roe<LinkDeviceImportResult> Import(IdentityStore& identity, DataKeyVault& vault,
                                            const std::string& bundle_json, const SecretText& pin,
                                            int64_t now_ms);
};

} // namespace pbr
