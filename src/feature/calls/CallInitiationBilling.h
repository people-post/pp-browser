#pragma once

#include "common/Error.h"
#include "common/Module.h"
#include "domain/messaging/CallTypes.h"
#include "domain/messaging/InitiationBillingStore.h"
#include "domain/people/IdentityStore.h"
#include "foundation/data/PricingTypes.h"

#include <cstdint>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * P001 call-initiation pricing on the call's invite / accept: the offer an invite carries to a peer
 * not yet open, the payable checks (outbound offer, take-all on accept), our floor against an
 * inbound offer, and the per-peer book (offered / open). No store bound: calls are free and carry
 * no pricing. The call session workflow asks at each step. Calls owner.
 */
class CallInitiationBilling : public Module {
public:
  explicit CallInitiationBilling(IdentityStore& identity);

  void SetStore(InitiationBillingStore* store) { store_ = store; }
  InitiationBillingStore* Store() const { return store_; }

  /** Placing a call to `invitees`: refused when an offer is due and it cannot be paid. */
  Roe<void> CheckCanPlace(const std::vector<std::string>& invitees, const std::string& local_identity) const;
  /** The offer `invitee` gets (none once open), onto `invite`; refused when it cannot be paid. */
  Roe<void> FillInviteOffer(const std::string& invitee, CallInviteDetail& invite) const;
  /** The invite went out: book its offer. */
  void NoteInviteSent(const std::string& invitee, const CallInviteDetail& invite);

  /** The offer `peer_identity` made us (0: none). */
  int64_t OfferFrom(const std::string& peer_identity) const;
  /** Accepting with `decision` on `offer_minor`: refused when take-all cannot be collected. */
  static Roe<void> CheckCanAccept(InitiationChargeDecision decision, int64_t offer_minor);
  /** Our accept carries the offer and our decision (with a store bound). */
  void FillAccept(int64_t offer_minor, InitiationChargeDecision decision, CallAcceptDetail& accept) const;
  /** Our accept went out: `inviter` is open. */
  void NoteAccepted(const std::string& inviter);

  /** An inbound invite: false when its offer is below our floor (decline it); else its offer is booked. */
  bool TakeInboundOffer(const std::string& sender_identity, const CallInviteDetail& invite);

private:
  IdentityStore& identity_;
  InitiationBillingStore* store_ = nullptr;
};

} // namespace pbr
