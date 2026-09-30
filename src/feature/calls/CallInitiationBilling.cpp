#include "feature/calls/CallInitiationBilling.h"

#include "domain/messaging/InitiationPricing.h"

#include "common/PbrCompat.h"

namespace pbr {

CallInitiationBilling::CallInitiationBilling(IdentityStore& identity) : identity_(identity) {
  redirectLogger("CallInitiationBilling");
}

Roe<void> CallInitiationBilling::CheckCanPlace(const std::vector<std::string>& invitees,
                                               const std::string& local_identity) const {
  if (!store_) {
    return {};
  }
  for (const std::string& invitee : invitees) {
    if (invitee.empty() || invitee == local_identity || store_->IsOpen(invitee)) {
      continue;
    }
    const int64_t offer = InitiationPricing::DefaultOfferForFloor(store_->Get(invitee).floor_minor);
    if (auto payable = InitiationPricing::CheckOutboundPayable(offer); !payable) {
      return payable.error();
    }
  }
  return {};
}

Roe<void> CallInitiationBilling::FillInviteOffer(const std::string& invitee, CallInviteDetail& invite) const {
  if (!store_ || store_->IsOpen(invitee)) {
    return {};
  }
  const InitiationPeerBilling billing = store_->Get(invitee);
  const int64_t offer = InitiationPricing::DefaultOfferForFloor(billing.floor_minor);
  if (auto payable = InitiationPricing::CheckOutboundPayable(offer); !payable) {
    return payable.error();
  }
  invite.offer_amount_minor = offer;
  invite.floor_minor = billing.floor_minor;
  invite.currency = kPricingCurrencyId;
  return {};
}

void CallInitiationBilling::NoteInviteSent(const std::string& invitee, const CallInviteDetail& invite) {
  if (store_ && invite.offer_amount_minor > 0) {
    (void)store_->MarkOffered(invitee, invite.offer_amount_minor, invite.floor_minor);
  }
}

int64_t CallInitiationBilling::OfferFrom(const std::string& peer_identity) const {
  if (!store_ || peer_identity.empty()) {
    return 0;
  }
  return store_->Get(peer_identity).offer_minor;
}

Roe<void> CallInitiationBilling::CheckCanAccept(const InitiationChargeDecision decision, const int64_t offer_minor) {
  if (decision == InitiationChargeDecision::TakeAll && offer_minor > 0 && !PaymentRailsAvailable()) {
    return Error("payment_unavailable: cannot collect charge yet");
  }
  return {};
}

void CallInitiationBilling::FillAccept(const int64_t offer_minor, const InitiationChargeDecision decision,
                                       CallAcceptDetail& accept) const {
  // P001: the recipient chooses waive (0) or take_all (rails checked before).
  if (store_) {
    accept.offer_amount_minor = offer_minor;
    accept.charge_decision = InitiationChargeDecisionToWire(decision);
  }
}

void CallInitiationBilling::NoteAccepted(const std::string& inviter) {
  if (store_) {
    (void)store_->MarkOpen(inviter);
  }
}

bool CallInitiationBilling::TakeInboundOffer(const std::string& sender_identity, const CallInviteDetail& invite) {
  if (!store_) {
    return true;
  }
  int64_t local_floor = 0;
  if (auto id = identity_.Get()) {
    local_floor = id->initiation_floor;
  }
  if (local_floor <= 0) {
    return true;
  }
  // P001: when we charge (local floor > 0), offers below it are refused.
  if (auto ok = InitiationPricing::CheckOfferAgainstFloor(invite.offer_amount_minor, local_floor); !ok) {
    log().info << "CallInvite rejected offer_too_low call_id=" << invite.call_id
               << " offer=" << invite.offer_amount_minor << " floor=" << local_floor;
    return false;
  }
  (void)store_->MarkOffered(sender_identity, invite.offer_amount_minor, local_floor);
  return true;
}

} // namespace pbr
