#include "feature/calls/CallMediaPaths.h"

namespace pbr {

CallDirectPath::CallDirectPath(Ops ops) : ops_(std::move(ops)) {}

void CallDirectPath::ScheduleStart(const std::string& call_id, const std::string& peer_identity,
                                   bool offerer) {
  if (!ops_.schedule_start || call_id.empty()) {
    return;
  }
  if (ops_.acquire) {
    (void)ops_.acquire(call_id);
  }
  ops_.schedule_start(call_id, peer_identity, offerer);
}

Roe<void> CallDirectPath::ReleaseTransport(const CallMediaSeat::Token& token) {
  if (ops_.allows_path_op && !ops_.allows_path_op(token)) {
    return {};
  }
  if (!ops_.release_transport) {
    return Error("direct path unavailable");
  }
  if (ops_.note_path) {
    ops_.note_path(CallMediaSeat::PathKind::Hop);
  }
  ops_.release_transport(token);
  return {};
}

CallHopPath::CallHopPath(Ops ops) : ops_(std::move(ops)) {}

CallMediaSeat::Token CallHopPath::BindForAttach(const std::string& call_id) {
  if (!ops_.acquire || call_id.empty()) {
    return {};
  }
  return ops_.acquire(call_id);
}

bool CallHopPath::Allows(const CallMediaSeat::Token& token) const {
  return ops_.allows_path_op && ops_.allows_path_op(token);
}

} // namespace pbr
