#include "feature/calls/CallMediaCoordinator.h"

#include "common/Logger.h"

#include <utility>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

logging::Logger& CallMediaCoordinatorLog() {
  static logging::Logger logger = logging::getLogger("CallMediaCoordinator");
  return logger;
}

const char* PathName(const CallMediaSeat::PathKind path) {
  switch (path) {
  case CallMediaSeat::PathKind::None:
    return "none";
  case CallMediaSeat::PathKind::Direct:
    return "direct";
  case CallMediaSeat::PathKind::Hop:
    return "hop";
  }
  return "?";
}

} // namespace

CallMediaCoordinator::CallMediaCoordinator(std::string call_id, const CallMediaResources& resources)
    : call_id_(std::move(call_id)), resources_(resources) {}

Roe<void> CallMediaCoordinator::StartEngine(const CallMediaSeat::PathKind path, CallMediaEngine::SfuSendFn send) {
  CallMediaSeat* const seat = resources_.seat;
  if (seat && !seat->AllowsPathOp(seat->Acquire(call_id_))) {
    return Error("media seat will not hold call " + call_id_);
  }
  if (auto started = Engine().StartSfu(call_id_, std::move(send)); !started) {
    return started;
  }
  if (seat) {
    seat->NoteStart(call_id_);
    seat->NotePath(path);
    // NoteStart bumps the epoch; the call must still hold the seat (a concurrent release lost it).
    if (!seat->IsBound(call_id_)) {
      Engine().Stop();
      return Error("media seat lost call " + call_id_ + " during start");
    }
  }
  path_ = path;
  CallMediaCoordinatorLog().info << "engine started call_id=" << call_id_ << " path=" << PathName(path);
  return {};
}

void CallMediaCoordinator::StopEngine(const char* why) {
  const std::string running = Engine().ActiveCallId();
  if (!running.empty() && running != call_id_) {
    // Parity: the stop paths always stopped leftover media (a drifted engine is worse than a stop).
    CallMediaCoordinatorLog().warning << "stopping engine running call_id=" << running << " for call_id=" << call_id_
                                      << " (" << (why ? why : "") << ")";
  }
  Engine().Stop();
}

Roe<void> CallMediaCoordinator::BeginDirect(const std::string& peer_identity, const bool offerer) {
  if (!resources_.direct) {
    return Error("direct media path unavailable");
  }
  if (resources_.seat) {
    (void)resources_.seat->Acquire(call_id_);
  }
  CallMediaCoordinatorLog().info << "begin direct call_id=" << call_id_
                                 << " role=" << (offerer ? "offerer" : "answerer");
  resources_.direct->ScheduleDirectStart(call_id_, peer_identity, offerer);
  return {};
}

void CallMediaCoordinator::HoldSeatForHop() {
  if (resources_.seat) {
    (void)resources_.seat->Acquire(call_id_);
  }
}

void CallMediaCoordinator::ReleaseDirect() {
  if (!resources_.direct) {
    return;
  }
  CallMediaSeat* const seat = resources_.seat;
  if (!seat) {
    resources_.direct->ReleaseDirectTransport();
    return;
  }
  const CallMediaSeat::Token token = seat->CurrentToken();
  // Another call took the seat (or none holds it): this call's 1:1 transport is not ours to drop now.
  if (token.call_id != call_id_ || !seat->AllowsPathOp(token)) {
    return;
  }
  seat->NotePath(CallMediaSeat::PathKind::Hop);
  resources_.direct->ReleaseDirectTransport(token);
}

} // namespace pbr
