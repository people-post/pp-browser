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

void LiveCallMedia::BindMediaResources(MediaEngine* engine, CallMediaSeat* seat) {
  resources_.engine = engine;
  resources_.seat = seat;
}

CallMediaCoordinator* LiveCallMedia::Find(const std::string& call_id) {
  Sweep();
  auto it = media_.find(call_id);
  return it == media_.end() ? nullptr : it->second.coordinator.get();
}

const CallMediaCoordinator* LiveCallMedia::Peek(const std::string& call_id) const {
  auto it = media_.find(call_id);
  return it == media_.end() ? nullptr : it->second.coordinator.get();
}

void LiveCallMedia::DropDirectPaths() {
  for (auto& [id, entry] : media_) {
    if (entry.coordinator) {
      entry.coordinator->DropDirect();
    }
  }
}

void LiveCallMedia::ForEachHopPath(const std::function<void(CallHopDriver&)>& fn) {
  for (auto& [id, entry] : media_) {
    if (CallHopDriver* hop = entry.coordinator ? entry.coordinator->HopIfBuilt() : nullptr) {
      fn(*hop);
    }
  }
}

void LiveCallMedia::ForEachDirectPath(const std::function<void(CallDirectDriver&)>& fn) {
  for (auto& [id, entry] : media_) {
    if (CallDirectDriver* direct = entry.coordinator ? entry.coordinator->DirectIfBuilt() : nullptr) {
      fn(*direct);
    }
  }
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
  CallMediaCoordinator* call = Find(call_id);
  if (CallHopDriver* hop = call ? call->HopIfBuilt() : nullptr) {
    hop->OnMediaStopped(call_id);
  }
  if (CallDirectDriver* direct = call ? call->DirectIfBuilt() : nullptr) {
    direct->StopMeshMedia(call_id);
  } else if (resources_.engine && (resources_.engine->IsActive() || resources_.engine->IsSfuMode())) {
    resources_.engine->Stop();
  }
}

bool LiveCallMedia::MediaRunning() const {
  return resources_.engine && resources_.engine->IsActive();
}

std::string LiveCallMedia::MediaRunningCallId() const {
  return resources_.engine ? resources_.engine->ActiveCallId() : std::string{};
}

void LiveCallMedia::StopMediaExcept(const std::string& keep_call_id) {
  MediaEngine* engine = resources_.engine;
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
