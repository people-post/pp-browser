#pragma once

#include "lib/people/AccountCard.h"

#include "common/Error.h"
#include "common/ExtArea.h"
#include "common/ProtocolIds.h"

#include <cstdint>
#include <optional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

// The Amp app protocol it travels on, kAccountCardProtocolId, is common/ProtocolIds.h's.

/**
 * `get_card`: whose card is asked for, and who asks — signed with the requester's account key over its
 * device PeerId and the time (PeerRequestAuth v2, release-compatibility SIGNED_FORMATS § 2.7), and carrying the
 * requester's key card so the answering device can check a stranger with no lookup. The auth fields ride the
 * request's `auth` object.
 */
struct AccountCardRequest {
  std::string account_id;
  std::string requester_account_id;
  std::string requester_peer_id;
  int64_t timestamp_ms = 0;
  /** The typed signature, base64. */
  std::string signature;
  std::optional<KeyCard> requester_keys;
  /** `auth.auth_version`, read before anything else (missing: none sent); PeerRequestAuth::Sign sets it. */
  std::optional<int64_t> auth_version;
  /** The signed extension area (`auth.ext_b64`): empty at release 1, kept as received. */
  pp::ExtArea ext;
  /** Set when `auth.ext_b64` did not read (why): refused as malformed, after the version (PeerRequestAuth::Verify). */
  std::string ext_error;
};

/**
 * The error a server answers a request it cannot read with (a newer `auth_version`, an algorithm id or a critical
 * extension it does not know: release-compatibility SIGNED_FORMATS § 2.7), as `{"ok":false,"code":"update_needed"}`.
 */
inline constexpr char kAccountCardUpdateNeeded[] = "update_needed";

/** The answer: the account card, the key card (unauthenticated asker, audience Everyone), or neither. */
struct AccountCardReply {
  std::optional<AccountCard> card;
  std::optional<KeyCard> key_card;
  /** Set when there is no card to give ("not_available": not this account, not allowed). */
  std::string error;
};

std::string EncodeAccountCardRequest(const AccountCardRequest& request);
Roe<AccountCardRequest> DecodeAccountCardRequest(const std::string& json);
std::string EncodeAccountCardReply(const AccountCardReply& reply);
Roe<AccountCardReply> DecodeAccountCardReply(const std::string& json);

} // namespace pbr
