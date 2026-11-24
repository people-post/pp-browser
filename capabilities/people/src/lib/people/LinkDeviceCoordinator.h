#pragma once

#include "common/Secret.h"

#include "foundation/crypto/DataKeyVault.h"
#include "foundation/crypto/ProfileSecretsEngine.h"
#include "lib/people/IdentityStore.h"
#include "lib/people/LinkDeviceExchange.h"

#include "common/Error.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include "common/PbrCompat.h"

namespace pbr {

/** Linking a device and restoring from the phrase (M027, M023 §8) over the profile's identity and vault. */
class LinkDeviceCoordinator {
public:
  /** The main device's sealed reply to `offer` (LinkDeviceExchange::SealReply over the loaded identity). */
  static Roe<ByteVector> SealReply(IdentityStore& identity, std::span<const uint8_t> offer,
                                   std::optional<RelayAddress> relay, int64_t now_ms, int64_t ttl_ms = kLinkWindowMs);

  /**
   * The account of `seed` onto this install (LinkDeviceExchange::ImportAccount), then the DEK to every store
   * (`secrets` may be null in tests).
   */
  static Roe<LocalIdentity> ImportAccount(IdentityStore& identity, DataKeyVault& vault, ProfileSecretsEngine* secrets,
                                          const ByteVector& seed, std::string_view card_intent,
                                          const std::string& nickname, const SecretText& pin);
};

} // namespace pbr
