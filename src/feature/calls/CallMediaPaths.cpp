#include "feature/calls/CallMediaPaths.h"

#include "feature/calls/CallMediaBridge.h"
#include "feature/calls/CallTopologyController.h"

namespace pbr {

CallDirectPath::CallDirectPath(CallMediaBridge* bridge, CallMediaSeat* seat)
    : bridge_(bridge), seat_(seat) {}

void CallDirectPath::ScheduleStart(const std::string& call_id, const std::string& peer_identity,
                                   bool offerer) {
  if (!bridge_ || call_id.empty()) {
    return;
  }
  if (seat_) {
    (void)seat_->Acquire(call_id);
  }
  if (offerer) {
    bridge_->ScheduleStartMediaAsOfferer(call_id, peer_identity);
  } else {
    bridge_->ScheduleStartMediaAsAnswerer(call_id, peer_identity);
  }
}

Roe<void> CallDirectPath::ReleaseTransport(const CallMediaSeat::Token& token) {
  if (seat_ && !seat_->AllowsPathOp(token)) {
    return {};
  }
  if (!bridge_) {
    return Error("direct path unavailable");
  }
  if (seat_) {
    seat_->NotePath(CallMediaSeat::PathKind::Hop);
  }
  bridge_->ReleaseDirectTransport(token);
  return {};
}

CallHopPath::CallHopPath(CallTopologyController* topology, CallMediaSeat* seat)
    : topology_(topology), seat_(seat) {
  (void)topology_;
}

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
