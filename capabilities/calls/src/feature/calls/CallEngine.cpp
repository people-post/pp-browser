#include "feature/calls/CallEngine.h"

#include "common/PbrCompat.h"

namespace pbr {

bool CallEngine::Runs() const {
  return engine_.IsActive() && engine_.ActiveSessionId() == call_id_;
}

bool CallEngine::RunsOn(const CallMediaPath path) const {
  return started_on_.load(std::memory_order_acquire) == static_cast<int>(path) && Runs();
}

bool CallEngine::RunsAnother() const {
  if (!engine_.IsActive()) {
    return false;
  }
  const std::string running = engine_.ActiveSessionId();
  return !running.empty() && running != call_id_;
}

bool CallEngine::Deliver(const MediaEngine::MediaPacket& packet) {
  if (!Runs()) {
    return false;
  }
  engine_.OnInboundPacket(packet);
  return true;
}

void CallEngine::NoteOutboundDrop() {
  if (Runs()) {
    engine_.NoteOutboundDrop();
  }
}

void CallEngine::NoteUplinkBudget(const int64_t per_user_up_bps) {
  if (Runs()) {
    engine_.NoteUplinkBudget(per_user_up_bps);
  }
}

void CallEngine::ApplyTargets(const MediaTargets& targets) {
  if (Runs()) {
    engine_.ApplyTargets(targets);
  }
}

} // namespace pbr
