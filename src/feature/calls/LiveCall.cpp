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

bool IsLiveStatus(const CallMediaStatus status) {
  return status == CallMediaStatus::DirectLive || status == CallMediaStatus::HopLive;
}

bool IsConnectingStatus(const CallMediaStatus status) {
  switch (status) {
  case CallMediaStatus::DirectConnecting:
  case CallMediaStatus::HopWaiting:
  case CallMediaStatus::HopAttaching:
  case CallMediaStatus::Migrating:
  case CallMediaStatus::Reconnecting:
  case CallMediaStatus::DegradedTxOnly:
    return true;
  default:
    return false;
  }
}

bool AllowsDirect(const CallMediaStatus status) {
  return status == CallMediaStatus::DirectConnecting || status == CallMediaStatus::DegradedTxOnly ||
         status == CallMediaStatus::DirectLive || status == CallMediaStatus::Reconnecting;
}

bool AllowsHop(const CallMediaStatus status) {
  return status == CallMediaStatus::HopWaiting || status == CallMediaStatus::HopAttaching ||
         status == CallMediaStatus::HopLive || status == CallMediaStatus::Migrating;
}

CallArmedPlanner PlannerFor(const CallMediaStatus status) {
  switch (status) {
  case CallMediaStatus::Deciding:
    return CallArmedPlanner::Lifecycle;
  case CallMediaStatus::DirectConnecting:
  case CallMediaStatus::DirectLive:
  case CallMediaStatus::DegradedTxOnly:
  case CallMediaStatus::Reconnecting:
    return CallArmedPlanner::Bridge;
  case CallMediaStatus::HopWaiting:
  case CallMediaStatus::HopAttaching:
  case CallMediaStatus::HopLive:
  case CallMediaStatus::Migrating:
    return CallArmedPlanner::Topology;
  case CallMediaStatus::None:
  case CallMediaStatus::Failed:
    return CallArmedPlanner::None;
  }
  return CallArmedPlanner::None;
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

CallPhase LiveCall::Phase() const {
  switch (state_) {
  case LiveCallState::Ringing:
    return CallPhase::Ringing;
  case LiveCallState::Accepting:
    return CallPhase::Accepting;
  case LiveCallState::Ended:
    return CallPhase::Idle;
  case LiveCallState::Calling:
  case LiveCallState::Joined:
    break;
  }
  if (progress_.status == CallMediaStatus::Failed) {
    return CallPhase::ConnectFailed;
  }
  if (progress_.reached_live) {
    return CallPhase::InCall;
  }
  if (state_ == LiveCallState::Calling) {
    return CallPhase::OutboundCalling;  // nobody answered yet
  }
  if (progress_.key_pending) {
    return CallPhase::MediaPending;
  }
  if (IsConnectingStatus(progress_.status) || origin_ == LiveCallOrigin::Placed) {
    return CallPhase::MediaConnecting;
  }
  return CallPhase::JoinedLocal;
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

const LiveCall* LiveCalls::Shown() const {
  if (const LiveCall* active = Active()) {
    return active;
  }
  return TheRing();
}

std::string LiveCalls::AcceptingCallId() const {
  for (const auto& [id, call] : calls_) {
    if (call.state_ == LiveCallState::Accepting) {
      return id;
    }
  }
  return {};
}

bool LiveCalls::HasOtherActive(const std::string& call_id) const {
  for (const auto& [id, call] : calls_) {
    if (id != call_id && IsActiveState(call.state_)) {
      return true;
    }
  }
  return false;
}

CallPhase LiveCalls::Phase() const {
  const LiveCall* shown = Shown();
  return shown ? shown->Phase() : CallPhase::Idle;
}

CallMediaStatus LiveCalls::Status(const std::string& call_id) const {
  const LiveCall* call = Resolve(call_id);
  return call ? call->progress_.status : CallMediaStatus::None;
}

CallArmedPlanner LiveCalls::ArmedPlanner(const std::string& call_id) const {
  return PlannerFor(Status(call_id));
}

bool LiveCalls::AllowsDirectPath(const std::string& call_id) const {
  return AllowsDirect(Status(call_id));
}

bool LiveCalls::AllowsHopPath(const std::string& call_id) const {
  return AllowsHop(Status(call_id));
}

bool LiveCalls::SoftMigrateMayArm() const {
  return pbr::SoftMigrateMayArm(Status());
}

LiveCall* LiveCalls::Resolve(const std::string& call_id) {
  LiveCall* call = call_id.empty() ? Active() : Find(call_id);
  return call && call->IsOpen() ? call : nullptr;
}

const LiveCall* LiveCalls::Resolve(const std::string& call_id) const {
  return const_cast<LiveCalls*>(this)->Resolve(call_id);
}

void LiveCalls::Changed() const {
  if (on_changed_) {
    on_changed_();
  }
}

void LiveCalls::SetStatus(LiveCall& call, const CallMediaStatus next, const char* reason) {
  LiveCallMediaProgress& progress = call.progress_;
  const CallMediaStatus prev = progress.status;
  const CallPhase prev_phase = call.Phase();
  if (next == CallMediaStatus::Deciding && prev != CallMediaStatus::Deciding) {
    ++media_cancel_gen_;  // a new path decision: late work for the previous one aborts
  }
  progress.status = next;
  if (IsLiveStatus(next)) {
    progress.reached_live = true;
    progress.key_pending = false;
  } else if (next == CallMediaStatus::Failed) {
    progress.reached_live = false;
    progress.key_pending = false;
  }
  LiveCallLog().info << "status=" << CallMediaStatusName(prev) << "->" << CallMediaStatusName(next)
                     << " phase=" << CallPhaseName(prev_phase) << "->" << CallPhaseName(call.Phase())
                     << " reason=" << (reason ? reason : "") << " call_id=" << call.call_id_
                     << " cancel_gen=" << media_cancel_gen_ << " armed=" << CallArmedPlannerName(PlannerFor(next));
  Changed();
}

void LiveCalls::SetMediaStatus(const std::string& call_id, const CallMediaStatus status, const char* reason) {
  if (LiveCall* call = Resolve(call_id)) {
    SetStatus(*call, status, reason);
  }
}

void LiveCalls::ReportDirectProgress(const std::string& call_id, const CallDirectPlannerPhase phase) {
  if (const auto status = MediaStatusForDirectProgress(phase)) {
    SetMediaStatus(call_id, *status, "DirectProgress");
  }
}

void LiveCalls::ReportHopProgress(const std::string& call_id, const CallHopPlannerPhase phase) {
  if (const auto status = MediaStatusForHopProgress(phase)) {
    SetMediaStatus(call_id, *status, "HopProgress");
  }
}

void LiveCalls::RequestDirectArming(const std::string& call_id) {
  LiveCall* call = Resolve(call_id);
  if (call && !AllowsDirect(call->progress_.status) && ShouldHonorDirectArmingRequest(call->Phase())) {
    SetStatus(*call, CallMediaStatus::DirectConnecting, "RequestDirectArming");
  }
}

void LiveCalls::NoteOutboundStarted(const std::string& call_id) {
  // A fast accept can start the 1:1 path before this: never regress a running path to Deciding.
  LiveCall* call = Resolve(call_id);
  if (!call) {
    return;
  }
  const CallMediaStatus status = call->progress_.status;
  if (status == CallMediaStatus::None || status == CallMediaStatus::Deciding || status == CallMediaStatus::Failed) {
    SetStatus(*call, CallMediaStatus::Deciding, "OutboundStarted");
  }
}

void LiveCalls::NoteMediaDeferred(const std::string& call_id) {
  LiveCall* call = Resolve(call_id);
  if (!call) {
    return;
  }
  const CallPhase phase = call->Phase();
  if (phase == CallPhase::Accepting || phase == CallPhase::JoinedLocal || phase == CallPhase::OutboundCalling ||
      phase == CallPhase::MediaConnecting) {
    call->progress_.key_pending = true;
    LiveCallLog().info << "media waits for the key call_id=" << call->call_id_;
    Changed();
  }
}

void LiveCalls::NoteMediaKeyReady(const std::string& call_id) {
  LiveCall* call = Resolve(call_id);
  if (!call) {
    return;
  }
  const CallPhase phase = call->Phase();
  if (phase != CallPhase::MediaPending && phase != CallPhase::JoinedLocal && phase != CallPhase::Accepting) {
    return;
  }
  call->progress_.key_pending = false;
  const CallMediaStatus status = call->progress_.status;
  if (status == CallMediaStatus::None || status == CallMediaStatus::Deciding) {
    // Key ready without an explicit path yet: prefer Direct until a SoftMigrate.
    SetStatus(*call, CallMediaStatus::DirectConnecting, "MediaKeyReady");
    return;
  }
  Changed();
}

void LiveCalls::NoteMediaConnected(const std::string& call_id) {
  LiveCall* call = Resolve(call_id);
  if (!call) {
    return;
  }
  const CallMediaStatus status = call->progress_.status;
  if (status == CallMediaStatus::HopAttaching || status == CallMediaStatus::HopWaiting ||
      status == CallMediaStatus::HopLive || status == CallMediaStatus::Migrating) {
    SetStatus(*call, CallMediaStatus::HopLive, "Connected");
  } else if (status != CallMediaStatus::DirectLive) {
    SetStatus(*call, CallMediaStatus::DirectLive, "Connected");
  } else {
    call->progress_.reached_live = true;
    Changed();
  }
}

void LiveCalls::NoteMediaFailed(const std::string& call_id) {
  if (LiveCall* call = Resolve(call_id)) {
    SetStatus(*call, CallMediaStatus::Failed, "ConnectFailed");
  }
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
  Changed();
  return call;
}

void LiveCalls::MarkAccepting(const std::string& call_id) {
  LiveCall* call = Find(call_id);
  if (!call || call->state_ != LiveCallState::Ringing) {
    return;
  }
  WarnIfSecondActive(call_id);
  call->state_ = LiveCallState::Accepting;
  call->progress_ = {};
  LiveCallLog().info << "accepting call_id=" << call_id;
  Changed();
}

void LiveCalls::MarkAcceptFailed(const std::string& call_id) {
  LiveCall* call = Find(call_id);
  if (!call || call->state_ != LiveCallState::Accepting) {
    return;
  }
  call->state_ = LiveCallState::Ringing;
  LiveCallLog().info << "accept failed, ringing again call_id=" << call_id;
  Changed();
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
  const bool our_accept = call->origin_ == LiveCallOrigin::Invited;
  call->state_ = LiveCallState::Joined;
  if (our_accept && call->progress_.status == CallMediaStatus::None) {
    // Our accept landed: the call decides its path (Direct / Hop) — Deciding arms no planner yet.
    SetStatus(*call, CallMediaStatus::Deciding, "AcceptJoined");
    return;
  }
  Changed();
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
  ++media_cancel_gen_;  // late path work for the closed call aborts
  ended_order_.push_back(call_id);
  PruneEnded();
  Changed();
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
