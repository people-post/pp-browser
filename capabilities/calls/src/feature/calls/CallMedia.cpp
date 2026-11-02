#include "feature/calls/CallMedia.h"

#include "common/Logger.h"

#include <utility>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

logging::Logger& CallMediaLog() {
  static logging::Logger logger = logging::getLogger("CallMedia");
  return logger;
}

const char* PathName(const CallMediaPath path) {
  switch (path) {
    case CallMediaPath::Undecided:
      return "undecided";
    case CallMediaPath::Direct:
      return "direct";
    case CallMediaPath::Hop:
      return "hop";
  }
  return "?";
}

} // namespace

CallMedia::CallMedia(std::string call_id, CallMediaResources& resources)
    : call_id_(std::move(call_id)), resources_(resources) {}

CallMedia::~CallMedia() = default;

CallHopDriver* CallMedia::Hop() {
  if (!hop_ && resources_.make_hop) {
    hop_serial_ = ++resources_.next_path_serial;
    hop_ = resources_.make_hop(call_id_, hop_serial_, hop_carries_media_);
    if (hop_) {
      CallMediaLog().info << "group path built call_id=" << call_id_ << " serial=" << hop_serial_;
    }
  }
  return hop_.get();
}

CallDirectDriver* CallMedia::Direct() {
  if (!direct_ && resources_.make_direct) {
    direct_serial_ = ++resources_.next_path_serial;
    direct_ = resources_.make_direct(call_id_, direct_serial_, hop_carries_media_);
    if (direct_) {
      CallMediaLog().info << "1:1 path built call_id=" << call_id_ << " serial=" << direct_serial_;
    }
  }
  return direct_.get();
}

void CallMedia::DropDirect() {
  direct_.reset();
  direct_serial_ = 0;
}

Roe<void> CallMedia::StartEngine(const CallMediaPath path, MediaEngine::SfuSendFn send) {
  CallMediaSeat* const seat = resources_.seat;
  if (seat && !seat->AllowsPathOp(seat->Acquire(call_id_))) {
    return Error("media seat will not hold call " + call_id_);
  }
  if (auto started = Engine().StartSfu(call_id_, std::move(send)); !started) {
    return started;
  }
  if (seat) {
    seat->NoteStart(call_id_);
    // NoteStart bumps the epoch; the call must still hold the seat (a concurrent release lost it).
    if (!seat->IsBound(call_id_)) {
      Engine().Stop();
      return Error("media seat lost call " + call_id_ + " during start");
    }
  }
  CallMediaLog().info << "engine started call_id=" << call_id_ << " path=" << PathName(path);
  return {};
}

void CallMedia::StopEngine(const char* why) {
  const std::string running = Engine().ActiveCallId();
  if (!running.empty() && running != call_id_) {
    // Parity: the stop paths always stopped leftover media (a drifted engine is worse than a stop).
    CallMediaLog().warning << "stopping engine running call_id=" << running << " for call_id=" << call_id_ << " ("
                           << (why ? why : "") << ")";
  }
  Engine().Stop();
}

Roe<void> CallMedia::BeginDirect(const std::string& peer_identity, const bool offerer) {
  CallDirectDriver* direct = Direct();
  if (!direct) {
    return Error("direct media path unavailable");
  }
  if (resources_.seat) {
    (void)resources_.seat->Acquire(call_id_);
  }
  CallMediaLog().info << "begin direct call_id=" << call_id_ << " role=" << (offerer ? "offerer" : "answerer");
  direct->ScheduleDirectStart(call_id_, peer_identity, offerer);
  return {};
}

void CallMedia::HoldSeatForHop() {
  if (resources_.seat) {
    (void)resources_.seat->Acquire(call_id_);
  }
}

void CallMedia::ReleaseDirect() {
  CallDirectDriver* direct = DirectIfBuilt();
  if (!direct) {
    return;
  }
  CallMediaSeat* const seat = resources_.seat;
  if (!seat) {
    direct->ReleaseDirectTransport();
    return;
  }
  const CallMediaSeat::Token token = seat->CurrentToken();
  // Another call took the seat (or none holds it): this call's 1:1 transport is not ours to drop now.
  if (token.call_id != call_id_ || !seat->AllowsPathOp(token)) {
    return;
  }
  direct->ReleaseDirectTransport(token);
}

CallMediaPath CallMedia::DecideOnLocalAccept(const size_t n_joined, const std::optional<std::string>& sfu_hint) {
  CallHopDriver* hop = Hop();
  const CallMediaPath path =
      hop && hop->OnLocalAcceptJoined(call_id_, n_joined, sfu_hint) ? CallMediaPath::Hop : CallMediaPath::Direct;
  CallMediaLog().info << "local accept call_id=" << call_id_ << " n=" << n_joined
                      << " mode=" << CallMediaPathName(path);
  return path;
}

CallMediaPath CallMedia::DecideOnRemoteAccept(const size_t n_joined, const std::string& joiner_identity,
                                              const CallMediaPath current) {
  CallHopDriver* hop = Hop();
  CallMediaPath path = CallMediaPath::Direct;
  if ((hop && hop->OnRemoteAcceptJoined(call_id_, n_joined, joiner_identity)) || current == CallMediaPath::Hop) {
    path = CallMediaPath::Hop; // a group call never goes back to 1:1
  }
  CallMediaLog().info << "remote accept call_id=" << call_id_ << " n=" << n_joined << " joiner=" << joiner_identity
                      << " mode=" << CallMediaPathName(path);
  return path;
}

bool CallMedia::HopAttached() const {
  return hop_ && hop_->IsSfuAttached();
}

bool CallMedia::HopInFlight() const {
  return hop_ && hop_->IsAwaitingSfuRecovery();
}

bool CallMedia::ExpectsHop() {
  CallHopDriver* hop = Hop(); // the store may already say "group" before the hop path ran
  return hop && hop->ExpectsGroupMedia(call_id_);
}

void CallMedia::ExpectHopAttach() {
  if (CallHopDriver* hop = Hop()) {
    hop->BeginSfuAttachWait(call_id_);
  }
}

void CallMedia::SetMuted(const bool muted) {
  Engine().SetMuted(muted);
}

Roe<void> CallMedia::SetCamera(const bool enabled, const int display_rotation_degrees) {
  if (hop_) {
    hop_->RefreshAdaptation(call_id_, enabled);
  }
  if (!enabled) {
    (void)Engine().SetCameraEnabled(false, 0);
    return {};
  }
  return Engine().SetCameraEnabled(true, display_rotation_degrees);
}

bool CallMedia::CameraOn() const {
  return Engine().IsCameraEnabled();
}

void CallMedia::RequestKeyframe() {
  Engine().RequestVideoKeyframe();
}

Roe<void> CallMedia::Retry() {
  CallDirectDriver* direct = DirectIfBuilt();
  return direct ? direct->RetryMeshMedia(call_id_) : Roe<void>(Error("direct media path unavailable"));
}

Roe<void> CallMedia::ResumeFromInbound() {
  CallDirectDriver* direct = DirectIfBuilt();
  return direct ? direct->ResumeMeshMediaFromInbound(call_id_) : Roe<void>(Error("direct media path unavailable"));
}

void CallMedia::OnMediaKeyReady() {
  if (direct_) {
    direct_->OnMediaKeyReady(call_id_);
  }
}

void CallMedia::OnMediaKeyInstalled() {
  if (hop_) {
    hop_->OnMediaKeyInstalled(call_id_);
  }
}

CallMediaDirectCallbacks CallMedia::AcceptInbound(const CallMediaInboundHello& hello) {
  CallDirectDriver* direct = Direct();
  return direct ? direct->AcceptInbound(hello) : CallMediaDirectCallbacks{};
}

void CallMedia::OnPeerGaveUpReaching() {
  if (direct_) {
    direct_->OnPeerGaveUpReaching(call_id_);
  }
}

void CallMedia::OnPathPolicyChanged() {
  if (direct_) {
    direct_->OnPathPolicyChanged(call_id_);
  }
}

void CallMedia::OnLocalNetworkMoved() {
  if (direct_) {
    direct_->OnLocalNetworkChanged();
  }
  if (hop_) {
    hop_->OnLocalNetworkMoved();
  }
}

void CallMedia::OnJoinedCountObserved(const size_t n_joined) {
  if (CallHopDriver* hop = Hop()) {
    hop->OnJoinedCountObserved(call_id_, n_joined);
  }
}

Roe<void> CallMedia::OnInboundSfuAttach(const CallSfuAttachDetail& attach, const std::string& sender) {
  CallHopDriver* hop = Hop();
  return hop ? hop->OnInboundSfuAttach(call_id_, attach, sender) : Roe<void>(Error("no group path for the call"));
}

void CallMedia::OnInboundSfuAttachFailed(const CallSfuAttachFailedDetail& detail) {
  if (hop_) {
    hop_->OnInboundSfuAttachFailed(detail);
  }
}

void CallMedia::OnInboundHopRefuse(const CallHopRefuseDetail& detail) {
  if (hop_) {
    hop_->OnInboundHopRefuse(detail);
  }
}

void CallMedia::ClearSfuAttachWait() {
  if (hop_) {
    hop_->ClearSfuAttachWait();
  }
}

void CallMedia::OnPeerMediaRelayCapLearned(const std::string& peer_id) {
  if (hop_) {
    hop_->OnPeerMediaRelayCapLearned(call_id_, peer_id);
  }
}

bool CallMedia::IsOnHop() const {
  return hop_ && hop_->IsOnSfuForCall(call_id_);
}

void CallMedia::OnMediaStopped() {
  if (hop_) {
    hop_->OnMediaStopped(call_id_);
  }
}

void CallMedia::StopMedia() {
  if (direct_) {
    direct_->StopMeshMedia(call_id_); // tears the 1:1 path down, engine included
    return;
  }
  if (Engine().IsActive() || Engine().IsSfuMode()) {
    Engine().Stop();
  }
}

void CallMedia::HandleDirectEvent(const uint64_t serial, DirectPathEvent& event) {
  if (direct_ && serial == direct_serial_) {
    direct_->Handle(event);
  }
}

void CallMedia::HandleHopEvent(const uint64_t serial, TopologyEvent& event) {
  if (hop_ && serial == hop_serial_) {
    hop_->Handle(event);
  }
}

CallMedia::View CallMedia::PathsView() const {
  View view;
  if (direct_) {
    view.direct = direct_->Status();
  }
  if (hop_) {
    view.hop_attached = hop_->IsSfuAttached();
    view.hop_in_flight = hop_->IsAwaitingSfuRecovery();
    view.recovery = hop_->Recovery();
    if (view.hop_attached) {
      view.hop_health = hop_->HopHealth();
    }
  }
  return view;
}

} // namespace pbr
