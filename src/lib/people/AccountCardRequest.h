#pragma once

#include "lib/people/AccountCard.h"

#include "common/Error.h"
#include "common/ProtocolIds.h"

#include <cstdint>
#include <optional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

// The Amp app protocol it travels on, kAccountCardProtocolId, is common/ProtocolIds.h's.

/**
 * `get_card`: whose card is asked for, and who asks — signed with the requester's account key over its
 * device PeerId and the time (PeerRequestAuth), and carrying the requester's key card so the answering
 * device can check a stranger with no lookup.
 */
struct AccountCardRequest {
  std::string account_id;
  std::string requester_account_id;
  std::string requester_peer_id;
  int64_t timestamp_ms = 0;
  std::string signature;
  std::optional<KeyCard> requester_keys;
};

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
