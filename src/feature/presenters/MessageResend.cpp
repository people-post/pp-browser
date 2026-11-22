#include "feature/presenters/MessageResend.h"

#include "feature/presenters/SendMark.h"

#include <utility>

namespace pbr {

namespace {

SendMarkTap DecideResendOnly(const SendMarkTapFacts& facts) {
  SendMarkTap tap;
  if (!facts.row || !facts.thread || !MayShowSendMark(*facts.row)) {
    return tap;
  }
  const ThreadMessage& row = *facts.row;
  if (facts.started_ms) {
    tap.kind = SendMarkTap::Kind::StillTrying; // never a second send beside one still alive
    return tap;
  }
  if (MarkOf(row, std::nullopt, facts.now_ms, facts.no_recipients) != SendMark::Failed) {
    return tap;
  }
  const Thread& thread = *facts.thread;
  if (thread.kind != ThreadKind::Direct && thread.kind != ThreadKind::Group) {
    return tap;
  }
  // A private chat's strict numbering is not sealed again (StoredResendAllowed); an "@ai++" question is not either.
  if ((thread.kind == ThreadKind::Direct && thread.channel == ThreadChannel::E2e) || row.ai_invoke_mode) {
    tap.kind = SendMarkTap::Kind::ReasonOnly;
    tap.refusal = SendMarkTap::Refusal::ThisKind;
    return tap;
  }
  // B236: it goes again as itself — the same id, so a recipient that has it already keeps it once (a new message would
  // reach it twice); nothing is uploaded again (a picture's message points at the file already on the server).
  tap.kind = SendMarkTap::Kind::ResendSameMessage;
  return tap;
}

} // namespace

SendMarkTap DecideSendMarkTap(const SendMarkTapFacts& facts) {
  SendMarkTap tap = DecideResendOnly(facts);
  if (!facts.offer_fix || !facts.thread || facts.thread->kind != ThreadKind::Direct || !facts.peer_contact_id ||
      !facts.failure) {
    return tap;
  }
  // Only a message that is red now: not still trying, not gone.
  if (tap.kind != SendMarkTap::Kind::ResendSameMessage && tap.kind != SendMarkTap::Kind::ReasonOnly) {
    return tap;
  }
  const bool resendable = tap.kind == SendMarkTap::Kind::ResendSameMessage;
  if (*facts.failure == SendFailure::RecipientNotOnRelay) {
    SendMarkTap fix;
    fix.kind = SendMarkTap::Kind::UpdateTheirCard;
    fix.contact_id = *facts.peer_contact_id;
    fix.resendable = resendable;
    return fix;
  }
  if (*facts.failure == SendFailure::SenderNotRegistered) {
    SendMarkTap fix;
    fix.kind = SendMarkTap::Kind::UpdateMyCard;
    fix.resendable = resendable;
    return fix;
  }
  return tap;
}

SendAdvice AdviceOf(const SendMarkTap& tap) {
  switch (tap.kind) {
    case SendMarkTap::Kind::UpdateTheirCard:
      return tap.resendable ? SendAdvice::UpdateTheirCardAndResend : SendAdvice::UpdateTheirCardOnly;
    case SendMarkTap::Kind::UpdateMyCard:
      return SendAdvice::UpdateMyCard;
    default:
      return SendAdvice::None;
  }
}

} // namespace pbr
