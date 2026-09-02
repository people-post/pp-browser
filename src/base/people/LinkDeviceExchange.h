#pragma once

#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/DataKeyVault.h"
#include "foundation/crypto/LinkDeviceCodec.h"
#include "base/people/IdentityStore.h"

#include "common/Error.h"

#include <string>
#include <string_view>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

struct LinkDeviceImportResult {
  LocalIdentity identity;
  std::vector<LinkDevicePublicPsk> public_psks;
};

/** Capture / apply `pp-browser-link-device-v1` (M012). UI and push re-attach stay in feature. */
class LinkDeviceExchange {
public:
  static Roe<LinkDeviceBundleV1> Capture(const LocalIdentity& identity, const ByteVector& dek,
                                         std::vector<LinkDevicePublicPsk> public_psks, int64_t now_ms,
                                         int64_t ttl_ms = kLinkDeviceDefaultTtlMs);
  static Roe<std::string> ExportJson(const LocalIdentity& identity, const ByteVector& dek,
                                     std::vector<LinkDevicePublicPsk> public_psks, int64_t now_ms,
                                     int64_t ttl_ms = kLinkDeviceDefaultTtlMs);

  /**
   * Apply `pp-browser-link-device-v1`: wrap the shared DEK, keep local device
   * Ed25519 / Peer ID, replace account ML-DSA + Account ID + account KEM + relay
   * binding. Product path is an empty vault (`CreateWithDek`). Existing-vault
   * `ReplaceWithDek` remains for tests / engine; identity must already be loaded
   * under the current DEK.
   */
  static Roe<LinkDeviceImportResult> Import(IdentityStore& identity, DataKeyVault& vault,
                                            const std::string& bundle_json, std::string_view pin,
                                            int64_t now_ms);
};

} // namespace pbr
