#pragma once

#include "foundation/crypto/CryptoTypes.h"
#include "common/Error.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * An invite link this device made (G021), kept to show it again (its text and QR): the roster holds only the secret's
 * hash, so the secret lives here — encrypted at rest under the profile's data key. Answering a join needs only the
 * roster (the hash); a link the roster no longer names is still known here, so its join is refused as revoked or
 * expired rather than unknown.
 */
struct KeptInviteLink {
  std::string link_id;
  std::string group_id;
  /** Empty when read while the profile is locked. */
  ByteVector secret;
  std::string maker;
  int64_t expires_at = 0;
  uint64_t max_uses = 0;
  int64_t created_at = 0;
};

/** Where a device keeps the invite links it made (CR024: the app's storage keeps them; tests use an in-memory one). */
class IGroupInviteLinkStore {
public:
  virtual ~IGroupInviteLinkStore() = default;

  /** Keep (or replace) a link; needs the data key (the secret is encrypted). */
  virtual Roe<void> Keep(const KeptInviteLink& link) const = 0;
  /** The link `link_id`; its secret empty when the data key is not set. */
  virtual Roe<std::optional<KeptInviteLink>> Find(const std::string& link_id) const = 0;
  /** The group's links, newest first. */
  virtual Roe<std::vector<KeptInviteLink>> ListForGroup(const std::string& group_id) const = 0;
  virtual Roe<void> Forget(const std::string& link_id) const = 0;
  /** Forget the links that expired before `before_ms` (their joins are refused as unknown from then on). */
  virtual Roe<void> ForgetExpiredBefore(int64_t before_ms) const = 0;
};

} // namespace pbr
