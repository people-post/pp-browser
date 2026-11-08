#include "feature/presenters/CallBackOffer.h"

namespace pbr {

void CallBackOffer::OnAccept(const CallScreen& ring, const std::string& call_id) {
  pending_.reset();
  taken_call_id_.clear();
  const CallScreen::EndingCall& ending = ring.ending;
  if (ring.kind != CallScreen::Kind::Ringing || !ring.conflict || call_id.empty() || ring.ring_call_id != call_id ||
      ending.group || ending.same_peer || ending.peer_identity.empty() || ending.thread_id.empty()) {
    return;
  }
  pending_ = ending;
  taken_call_id_ = call_id;
}

std::optional<CallScreen::EndingCall> CallBackOffer::OnFrame(const CallScreen& screen,
                                                             const std::string& live_call_id) {
  if (!pending_) {
    return std::nullopt;
  }
  // Still taking it (answering; an accept that did not go through leaves it ringing), or in it.
  const bool on_screen = (screen.kind == CallScreen::Kind::Answering || screen.kind == CallScreen::Kind::Ringing) &&
                         screen.ring_call_id == taken_call_id_;
  if (on_screen || (!live_call_id.empty() && live_call_id == taken_call_id_)) {
    return std::nullopt;
  }
  std::optional<CallScreen::EndingCall> offer;
  if (screen.kind == CallScreen::Kind::None && live_call_id.empty()) {
    offer = std::move(pending_);
  }
  pending_.reset();
  taken_call_id_.clear();
  return offer;
}

} // namespace pbr
