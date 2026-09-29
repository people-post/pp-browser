#include "feature/calls/LiveCall.h"

#include "common/Logger.h"
#include "common/Utilities.h"

#include <algorithm>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

logging::Logger& LiveCallLog() {
  static logging::Logger logger = logging::getLogger("LiveCall");
  return logger;
}

/** Ended calls kept findable (their end reason) for late readers. */
constexpr size_t kEndedKept = 16;

bool IsActiveState(const LiveCallState state) {
  return state == LiveCallState::Calling || state == LiveCallState::Accepting || state == LiveCallState::Joined;
}

} // namespace

const char* LiveCallStateName(const LiveCallState state) {
  switch (state) {
  case LiveCallState::Calling:
    return "Calling";
  case LiveCallState::Ringing:
    return "Ringing";
  case LiveCallState::Accepting:
    return "Accepting";
  case LiveCallState::Joined:
    return "Joined";
  case LiveCallState::Ended:
    return "Ended";
  }
  return "?";
}

const char* LiveCallEndReasonName(const LiveCallEndReason reason) {
  switch (reason) {
  case LiveCallEndReason::None:
    return "None";
  case LiveCallEndReason::LocalLeave:
    return "LocalLeave";
  case LiveCallEndReason::RemoteEnded:
    return "RemoteEnded";
  case LiveCallEndReason::Declined:
    return "Declined";
  case LiveCallEndReason::DeclinedByPeer:
    return "DeclinedByPeer";
  case LiveCallEndReason::Expired:
    return "Expired";
  case LiveCallEndReason::Unanswered:
    return "Unanswered";
  case LiveCallEndReason::Superseded:
    return "Superseded";
  case LiveCallEndReason::MediaUnavailable:
    return "MediaUnavailable";
  case LiveCallEndReason::Shutdown:
    return "Shutdown";
  case LiveCallEndReason::Orphaned:
    return "Orphaned";
  }
  return "?";
}

bool LiveCall::EndedByPeer() const {
  return state_ == LiveCallState::Ended && state_at_close_ != LiveCallState::Ringing &&
         (end_reason_ == LiveCallEndReason::RemoteEnded || end_reason_ == LiveCallEndReason::DeclinedByPeer);
}

std::optional<std::string> LiveCall::SolePeer() const {
  if (peers_.size() != 1) {
    return std::nullopt;
  }
  return peers_.front();
}

LiveCall* LiveCalls::Find(const std::string& call_id) {
  auto it = calls_.find(call_id);
  return it == calls_.end() ? nullptr : &it->second;
}

const LiveCall* LiveCalls::Find(const std::string& call_id) const {
  auto it = calls_.find(call_id);
  return it == calls_.end() ? nullptr : &it->second;
}

LiveCall* LiveCalls::Active() {
  for (auto& [id, call] : calls_) {
    if (IsActiveState(call.state_)) {
      return &call;
    }
  }
  return nullptr;
}

const LiveCall* LiveCalls::Active() const {
  return const_cast<LiveCalls*>(this)->Active();
}

std::vector<const LiveCall*> LiveCalls::Ringing() const {
  std::vector<const LiveCall*> out;
  for (const auto& [id, call] : calls_) {
    if (call.state_ == LiveCallState::Ringing) {
      out.push_back(&call);
    }
  }
  return out;
}

const LiveCall* LiveCalls::TheRing() const {
  const LiveCall* newest = nullptr;
  for (const auto& [id, call] : calls_) {
    if ((call.state_ == LiveCallState::Ringing || call.state_ == LiveCallState::Accepting) &&
        (!newest || call.instance_ > newest->instance_)) {
      newest = &call;
    }
  }
  return newest;
}

const LiveCall* LiveCalls::LastEnded() const {
  return ended_order_.empty() ? nullptr : Find(ended_order_.back());
}

LiveCall& LiveCalls::AdmitPlaced(const std::string& call_id, const std::vector<std::string>& peers) {
  return Admit(call_id, LiveCallOrigin::Placed, LiveCallState::Calling, peers);
}

LiveCall& LiveCalls::AdmitInvited(const std::string& call_id, const std::vector<std::string>& peers) {
  if (LiveCall* open = Find(call_id); open && open->IsOpen()) {
    return *open;  // redelivered invite
  }
  return Admit(call_id, LiveCallOrigin::Invited, LiveCallState::Ringing, peers);
}

LiveCall& LiveCalls::Admit(const std::string& call_id, const LiveCallOrigin origin, const LiveCallState state,
                           const std::vector<std::string>& peers) {
  if (IsActiveState(state)) {
    WarnIfSecondActive(call_id);
  }
  ended_order_.erase(std::remove(ended_order_.begin(), ended_order_.end(), call_id), ended_order_.end());
  LiveCall& call = calls_[call_id];
  call = LiveCall{};
  call.call_id_ = call_id;
  call.instance_ = next_instance_++;
  call.origin_ = origin;
  call.state_ = state;
  call.admitted_at_ms_ = util::NowUnixMs();
  for (const std::string& peer : peers) {
    if (!peer.empty() && std::find(call.peers_.begin(), call.peers_.end(), peer) == call.peers_.end()) {
      call.peers_.push_back(peer);
    }
  }
  LiveCallLog().info << "admit call_id=" << call_id << " instance=" << call.instance_
                     << " origin=" << (origin == LiveCallOrigin::Placed ? "placed" : "invited")
                     << " state=" << LiveCallStateName(state) << " peers=" << call.peers_.size();
  return call;
}

void LiveCalls::MarkAccepting(const std::string& call_id) {
  LiveCall* call = Find(call_id);
  if (!call || call->state_ != LiveCallState::Ringing) {
    return;
  }
  WarnIfSecondActive(call_id);
  call->state_ = LiveCallState::Accepting;
  LiveCallLog().info << "accepting call_id=" << call_id;
}

void LiveCalls::MarkAcceptFailed(const std::string& call_id) {
  LiveCall* call = Find(call_id);
  if (!call || call->state_ != LiveCallState::Accepting) {
    return;
  }
  call->state_ = LiveCallState::Ringing;
  LiveCallLog().info << "accept failed, ringing again call_id=" << call_id;
}

void LiveCalls::MarkJoined(const std::string& call_id) {
  LiveCall* call = Find(call_id);
  if (!call || !call->IsOpen() || call->state_ == LiveCallState::Joined) {
    return;
  }
  if (call->state_ == LiveCallState::Ringing) {
    WarnIfSecondActive(call_id);
  }
  LiveCallLog().info << "joined call_id=" << call_id << " from=" << LiveCallStateName(call->state_);
  call->state_ = LiveCallState::Joined;
}

void LiveCalls::AddPeer(const std::string& call_id, const std::string& identity) {
  LiveCall* call = Find(call_id);
  if (!call || !call->IsOpen() || identity.empty() ||
      std::find(call->peers_.begin(), call->peers_.end(), identity) != call->peers_.end()) {
    return;
  }
  call->peers_.push_back(identity);
}

void LiveCalls::RemovePeer(const std::string& call_id, const std::string& identity) {
  LiveCall* call = Find(call_id);
  if (!call || !call->IsOpen()) {
    return;
  }
  call->peers_.erase(std::remove(call->peers_.begin(), call->peers_.end(), identity), call->peers_.end());
}

void LiveCalls::Close(const std::string& call_id, const LiveCallEndReason reason) {
  LiveCall* call = Find(call_id);
  if (!call || !call->IsOpen()) {
    return;
  }
  LiveCallLog().info << "close call_id=" << call_id << " instance=" << call->instance_
                     << " was=" << LiveCallStateName(call->state_) << " reason=" << LiveCallEndReasonName(reason);
  call->state_at_close_ = call->state_;
  call->state_ = LiveCallState::Ended;
  call->end_reason_ = reason;
  ended_order_.push_back(call_id);
  PruneEnded();
}

void LiveCalls::BindMediaResources(CallMediaEngine* engine, CallMediaSeat* seat) {
  resources_.engine = engine;
  resources_.seat = seat;
}

CallMediaCoordinator* LiveCalls::Media(const std::string& call_id) {
  LiveCall* call = Find(call_id);
  if (!call) {
    LiveCallLog().warning << "media for a call not admitted here call_id=" << call_id;
    return nullptr;
  }
  if (!call->media_ && resources_.engine) {
    call->media_ = std::make_unique<CallMediaCoordinator>(call_id, resources_);
  }
  return call->media_.get();
}

void LiveCalls::StopMedia(const std::string& call_id) {
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

bool LiveCalls::MediaRunning() const {
  return resources_.engine && resources_.engine->IsActive();
}

std::string LiveCalls::MediaRunningCallId() const {
  return resources_.engine ? resources_.engine->ActiveCallId() : std::string{};
}

void LiveCalls::StopMediaExcept(const std::string& keep_call_id) {
  CallMediaEngine* engine = resources_.engine;
  if (!engine || (!engine->IsActive() && !engine->IsSfuMode())) {
    return;
  }
  const std::string leftover = engine->ActiveCallId();
  if (!leftover.empty() && leftover != keep_call_id) {
    LiveCallLog().info << "stopping leftover media call_id=" << leftover << " for=" << keep_call_id;
    StopMedia(leftover);
  } else if (leftover.empty()) {
    LiveCallLog().info << "stopping zombie engine (no call id) for=" << keep_call_id;
    StopMedia({});
  }
}

void LiveCalls::WarnIfSecondActive(const std::string& call_id) const {
  for (const auto& [id, call] : calls_) {
    if (id != call_id && IsActiveState(call.state_)) {
      LiveCallLog().warning << "second active call call_id=" << call_id << " while call_id=" << id << " is "
                            << LiveCallStateName(call.state_);
      return;
    }
  }
}

void LiveCalls::PruneEnded() {
  while (ended_order_.size() > kEndedKept) {
    calls_.erase(ended_order_.front());
    ended_order_.erase(ended_order_.begin());
  }
}

} // namespace pbr
