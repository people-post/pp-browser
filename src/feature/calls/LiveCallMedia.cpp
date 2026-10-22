#include "feature/calls/LiveCallMedia.h"

#include "common/Logger.h"

#include "common/PbrCompat.h"

namespace pbr {
namespace {

logging::Logger& LiveCallMediaLog() {
  static logging::Logger logger = logging::getLogger("LiveCall");
  return logger;
}

} // namespace

void LiveCallMedia::BindMediaResources(CallMediaEngine* engine, CallMediaSeat* seat) {
  resources_.engine = engine;
  resources_.seat = seat;
}

CallMediaCoordinator* LiveCallMedia::Media(const std::string& call_id) {
  Sweep();
  const LiveCall* call = live_.Find(call_id);
  if (!call) {
    LiveCallMediaLog().warning << "media for a call not admitted here call_id=" << call_id;
    return nullptr;
  }
  Entry& entry = media_[call_id];  // Sweep dropped an earlier admission's
  entry.instance = call->Instance();
  if (!entry.coordinator && resources_.engine) {
    entry.coordinator = std::make_unique<CallMediaCoordinator>(call_id, resources_);
  }
  return entry.coordinator.get();
}

void LiveCallMedia::StopMedia(const std::string& call_id) {
  if (resources_.seat) {
    resources_.seat->Release(call_id);  // Detach then engine Stop (seat teardown hooks)
    return;
  }
  if (resources_.hop) {
    resources_.hop->OnMediaStopped(call_id);
  }
  if (resources_.direct) {
    resources_.direct->StopMeshMedia(call_id);
  }
}

bool LiveCallMedia::MediaRunning() const {
  return resources_.engine && resources_.engine->IsActive();
}

std::string LiveCallMedia::MediaRunningCallId() const {
  return resources_.engine ? resources_.engine->ActiveCallId() : std::string{};
}

void LiveCallMedia::StopMediaExcept(const std::string& keep_call_id) {
  CallMediaEngine* engine = resources_.engine;
  if (!engine || (!engine->IsActive() && !engine->IsSfuMode())) {
    return;
  }
  const std::string leftover = engine->ActiveCallId();
  if (!leftover.empty() && leftover != keep_call_id) {
    LiveCallMediaLog().info << "stopping leftover media call_id=" << leftover << " for=" << keep_call_id;
    StopMedia(leftover);
  } else if (leftover.empty()) {
    LiveCallMediaLog().info << "stopping zombie engine (no call id) for=" << keep_call_id;
    StopMedia({});
  }
}

void LiveCallMedia::Sweep() {
  for (auto it = media_.begin(); it != media_.end();) {
    const LiveCall* call = live_.Find(it->first);
    it = (!call || call->Instance() != it->second.instance) ? media_.erase(it) : std::next(it);
  }
}

} // namespace pbr
