#pragma once

#include "common/ExtArea.h"

#include <cstdint>
#include <string>

namespace pbr {

/**
 * A recipient's grant to a sender (multi-relay MR005): its account's signed word that the sender is a contact. The
 * sender attaches it to every relay send into the recipient's inbox; the relay checks it with the recipient's key
 * and treats a send without one as a stranger's (a price, or a small first-contact quota and no push).
 *
 * Grant v2 (release-compatibility SIGNED_FORMATS § 2.4; docs/contracts/ACCOUNT_CARD.md § Relay grants): signed over
 * `"pp-browser:relay-grant" 00 ‖ u8 2 ‖ LenUtf8 recipient_account_id ‖ LenUtf8 sender_account_id ‖ i64 issued_at ‖
 * i64 expires_at ‖ ext area`; its JSON, `RelayGrantToJson` (lib/delivery/RelayGrantSigner.h), rides a send's `grant`, the control
 * lane's `relay_grant` notice and the account locker's `grants`.
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
  /** The recipient account key's typed signature (algorithm id, then ML-DSA-65), standard base64. */
  std::string signature;
  /** The signed extension area (`ext_b64`): none is defined at release 1; a received grant keeps its entries. */
  pp::ExtArea ext;

  bool operator==(const RelayGrant&) const = default;
};

/** A grant lasts at most this long; the recipient renews it while the sender stays a contact. */
inline constexpr int64_t kRelayGrantMaxLifetimeMs = 30LL * 24 * 60 * 60 * 1000;

/** The grant's version, outside (`grant_version`) and inside (the byte after the domain): release 1's only one. */
inline constexpr uint8_t kRelayGrantVersion = 2;
inline constexpr char kRelayGrantVersionKey[] = "grant_version";

} // namespace pbr
