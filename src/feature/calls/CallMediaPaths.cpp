#include "feature/calls/CallMediaPaths.h"

namespace pbr {

CallDirectPath::CallDirectPath(Ops ops, CallMediaSeat* seat) : ops_(std::move(ops)), seat_(seat) {}

void CallDirectPath::ScheduleStart(const std::string& call_id, const std::string& peer_identity,
                                   bool offerer) {
  if (!ops_.schedule_start || call_id.empty()) {
    return;
  }
  if (seat_) {
    (void)seat_->Acquire(call_id);
  }
  ops_.schedule_start(call_id, peer_identity, offerer);
}

Roe<void> CallDirectPath::ReleaseTransport(const CallMediaSeat::Token& token) {
  if (seat_ && !seat_->AllowsPathOp(token)) {
    return {};
  }
  if (!ops_.release_transport) {
    return Error("direct path unavailable");
  }
  if (seat_) {
    seat_->NotePath(CallMediaSeat::PathKind::Hop);
  }
  ops_.release_transport(token);
  return {};
}

CallHopPath::CallHopPath(CallMediaSeat* seat) : seat_(seat) {}

CallMediaSeat::Token CallHopPath::BindForAttach(const std::string& call_id) {
  if (!seat_ || call_id.empty()) {
    return {};
  }
  return seat_->Acquire(call_id);
}

bool CallHopPath::Allows(const CallMediaSeat::Token& token) const {
  return seat_ && seat_->AllowsPathOp(token);
}

} // namespace pbr
