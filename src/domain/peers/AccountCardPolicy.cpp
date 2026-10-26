#include "domain/peers/AccountCardPolicy.h"

#include <algorithm>

namespace pbr {

AccountCardReply AnswerCardRequest(const AccountCardRequest& request, const std::string& remote_peer_id,
                                   const CardServeContext& context) {
  const AccountCardReply not_available{.error = "not_available"};
  if (request.account_id.empty() || request.account_id != context.own_account_id) {
    return not_available;
  }
  const bool authenticated = context.authenticate && context.authenticate(request, remote_peer_id);
  if (authenticated && !(context.blocked && context.blocked(request.requester_account_id))) {
    if (auto card = context.own_card ? context.own_card() : Roe<AccountCard>(Error("no card"))) {
      return AccountCardReply{.card = *card};
    }
    return not_available;
  }
  if (!authenticated && context.audience_everyone && context.audience_everyone()) {
    if (auto keys = context.own_keys ? context.own_keys() : Roe<KeyCard>(Error("no keys"))) {
      return AccountCardReply{.key_card = *keys};
    }
  }
  return not_available;
}

Roe<AccountCard> AcceptCardReply(const AccountCardReply& reply, const std::string& account_id,
                                 const std::string& peer_id, const int64_t now_ms) {
  if (!reply.card) {
    return Error("account card: " + (reply.error.empty() ? std::string("not_available") : reply.error));
  }
  const AccountCard& card = *reply.card;
  if (card.keys.account_id != account_id) {
    return Error("account card: answered for another account");
  }
  if (!FindCardDevice(card, peer_id)) {
    return Error("account card: the answering device is not on it");
  }
  if (auto checked = VerifyAccountCard(card, now_ms); !checked) {
    return checked.error();
  }
  return card;
}

} // namespace pbr
