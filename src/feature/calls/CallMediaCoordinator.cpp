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

CallMediaCoordinator::CallMediaCoordinator(std::string call_id, CallMediaEngine& engine, CallMediaSeat* seat)
    : call_id_(std::move(call_id)), engine_(engine), seat_(seat) {}

Roe<void> CallMediaCoordinator::StartEngine(const CallMediaSeat::PathKind path, CallMediaEngine::SfuSendFn send) {
  if (seat_ && !seat_->AllowsPathOp(seat_->Acquire(call_id_))) {
    return Error("media seat will not hold call " + call_id_);
  }
  if (auto started = engine_.StartSfu(call_id_, std::move(send)); !started) {
    return started;
  }
  if (seat_) {
    seat_->NoteStart(call_id_);
    seat_->NotePath(path);
    // NoteStart bumps the epoch; the call must still hold the seat (a concurrent release lost it).
    if (!seat_->IsBound(call_id_)) {
      engine_.Stop();
      return Error("media seat lost call " + call_id_ + " during start");
    }
  }
  path_ = path;
  CallMediaCoordinatorLog().info << "engine started call_id=" << call_id_ << " path=" << PathName(path);
  return {};
}

void CallMediaCoordinator::StopEngine(const char* why) {
  const std::string running = engine_.ActiveCallId();
  if (!running.empty() && running != call_id_) {
    // Parity: the stop paths always stopped leftover media (a drifted engine is worse than a stop).
    CallMediaCoordinatorLog().warning << "stopping engine running call_id=" << running << " for call_id=" << call_id_
                                      << " (" << (why ? why : "") << ")";
  }
  engine_.Stop();
}

} // namespace pbr
