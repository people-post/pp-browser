#pragma once

#include "lib/messaging/AccountCardRequest.h"

#include "common/Error.h"

#include <cstdint>
#include <functional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/** What a device knows when it answers a get_card request (docs/contracts/ACCOUNT_CARD.md#card-exchange-over-amp). */
struct CardServeContext {
  std::string own_account_id;
  std::function<Roe<AccountCard>()> own_card;
  std::function<Roe<KeyCard>()> own_keys;
  /** PeerRequestAuth on the request, bound to the asking link. */
  std::function<Roe<void>(const AccountCardRequest& request, const std::string& remote_peer_id)> authenticate;
  std::function<bool(const std::string& account_id)> blocked;
  /** This account publishes its addresses to everyone (DirectAudience::Everyone). */
  std::function<bool()> audience_everyone;
};

/**
 * The answer to `request` from device `remote_peer_id`. Answering at all says this device is that
 * account's, so: our account only; the account card to an authenticated, unblocked requester; the key
 * card to anyone else only when our audience is Everyone; otherwise the same `not_available` for all.
 */
AccountCardReply AnswerCardRequest(const AccountCardRequest& request, const std::string& remote_peer_id,
                                   const CardServeContext& context);

/**
 * A reply from device `peer_id` to a request for `account_id`'s card: the account card when it proves
 * itself for that account and lists the answering device (the card is how a device is known to speak for
 * an account), else an error.
 */
Roe<AccountCard> AcceptCardReply(const AccountCardReply& reply, const std::string& account_id,
                                 const std::string& peer_id, int64_t now_ms);

} // namespace pbr
