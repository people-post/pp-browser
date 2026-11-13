#pragma once

#include <cstdint>
#include <string>

namespace pbr {

/**
 * A recipient's grant to a sender (multi-relay MR005): its account's signed word that the sender is a contact. The
 * sender attaches it to every relay send into the recipient's inbox; the relay checks it with the recipient's key
 * and treats a send without one as a stranger's (a price, or a small first-contact quota and no push). Signed over
 * `pp-browser:relay-grant-v1` (lib/delivery/RelayGrantSigner.h); docs/contracts/ACCOUNT_CARD.md § Relay grants.
 */
struct RelayGrant {
  /** Who grants: the inbox's account. */
  std::string recipient_account_id;
  /** Who may write to it. */
  std::string sender_account_id;
  /** Unix ms. */
  int64_t issued_at = 0;
  /** Unix ms; at most kRelayGrantMaxLifetimeMs after `issued_at`. */
  int64_t expires_at = 0;
  /** ML-DSA-65 by the recipient's account key, standard base64. */
  std::string signature;

  bool operator==(const RelayGrant&) const = default;
};

/** A grant lasts at most this long; the recipient renews it while the sender stays a contact. */
inline constexpr int64_t kRelayGrantMaxLifetimeMs = 30LL * 24 * 60 * 60 * 1000;

} // namespace pbr
