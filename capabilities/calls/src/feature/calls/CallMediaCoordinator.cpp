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

CallMediaCoordinator::CallMediaCoordinator(std::string call_id, CallMediaResources& resources)
    : call_id_(std::move(call_id)), resources_(resources) {}

CallMediaCoordinator::~CallMediaCoordinator() = default;

CallHopDriver* CallMediaCoordinator::Hop() {
  if (!hop_ && resources_.make_hop) {
    hop_serial_ = ++resources_.next_path_serial;
    hop_ = resources_.make_hop(call_id_, hop_serial_, hop_carries_media_);
    if (hop_) {
      CallMediaCoordinatorLog().info << "group path built call_id=" << call_id_ << " serial=" << hop_serial_;
    }
  }
  return hop_.get();
}

CallDirectDriver* CallMediaCoordinator::Direct() {
  if (!direct_ && resources_.make_direct) {
    direct_serial_ = ++resources_.next_path_serial;
    direct_ = resources_.make_direct(call_id_, direct_serial_, hop_carries_media_);
    if (direct_) {
      CallMediaCoordinatorLog().info << "1:1 path built call_id=" << call_id_ << " serial=" << direct_serial_;
    }
  }
  return direct_.get();
}

void CallMediaCoordinator::DropDirect() {
  direct_.reset();
  direct_serial_ = 0;
}

Roe<void> CallMediaCoordinator::StartEngine(const CallMediaPath path, MediaEngine::SfuSendFn send) {
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
  CallDirectDriver* direct = Direct();
  if (!direct) {
    return Error("direct media path unavailable");
  }
  if (resources_.seat) {
    (void)resources_.seat->Acquire(call_id_);
  }
  CallMediaCoordinatorLog().info << "begin direct call_id=" << call_id_
                                 << " role=" << (offerer ? "offerer" : "answerer");
  direct->ScheduleDirectStart(call_id_, peer_identity, offerer);
  return {};
}

void CallMediaCoordinator::HoldSeatForHop() {
  if (resources_.seat) {
    (void)resources_.seat->Acquire(call_id_);
  }
}

void CallMediaCoordinator::ReleaseDirect() {
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

CallMediaPath CallMediaCoordinator::DecideOnLocalAccept(const size_t n_joined,
                                                       const std::optional<std::string>& sfu_hint) {
  CallHopDriver* hop = Hop();
  const CallMediaPath path =
      hop && hop->OnLocalAcceptJoined(call_id_, n_joined, sfu_hint) ? CallMediaPath::Hop : CallMediaPath::Direct;
  CallMediaCoordinatorLog().info << "local accept call_id=" << call_id_ << " n=" << n_joined
                                 << " mode=" << CallMediaPathName(path);
  return path;
}

CallMediaPath CallMediaCoordinator::DecideOnRemoteAccept(const size_t n_joined, const std::string& joiner_identity,
                                                         const CallMediaPath current) {
  CallHopDriver* hop = Hop();
  CallMediaPath path = CallMediaPath::Direct;
  if ((hop && hop->OnRemoteAcceptJoined(call_id_, n_joined, joiner_identity)) || current == CallMediaPath::Hop) {
    path = CallMediaPath::Hop; // a group call never goes back to 1:1
  }
  CallMediaCoordinatorLog().info << "remote accept call_id=" << call_id_ << " n=" << n_joined
                                 << " joiner=" << joiner_identity << " mode=" << CallMediaPathName(path);
  return path;
}

bool CallMediaCoordinator::HopAttached() const {
  return hop_ && hop_->IsSfuAttached();
}

bool CallMediaCoordinator::HopInFlight() const {
  return hop_ && hop_->IsAwaitingSfuRecovery();
}

bool CallMediaCoordinator::ExpectsHop() {
  CallHopDriver* hop = Hop(); // the store may already say "group" before the hop path ran
  return hop && hop->ExpectsGroupMedia(call_id_);
}

void CallMediaCoordinator::ExpectHopAttach() {
  if (CallHopDriver* hop = Hop()) {
    hop->BeginSfuAttachWait(call_id_);
  }
}

void CallMediaCoordinator::SetMuted(const bool muted) {
  Engine().SetMuted(muted);
}

Roe<void> CallMediaCoordinator::SetCamera(const bool enabled, const int display_rotation_degrees) {
  if (hop_) {
    hop_->RefreshAdaptation(call_id_, enabled);
  }
  if (!enabled) {
    (void)Engine().SetCameraEnabled(false, 0);
    return {};
  }
  return Engine().SetCameraEnabled(true, display_rotation_degrees);
}

bool CallMediaCoordinator::CameraOn() const {
  return Engine().IsCameraEnabled();
}

void CallMediaCoordinator::RequestKeyframe() {
  Engine().RequestVideoKeyframe();
}

Roe<void> CallMediaCoordinator::Retry() {
  CallDirectDriver* direct = DirectIfBuilt();
  return direct ? direct->RetryMeshMedia(call_id_) : Roe<void>(Error("direct media path unavailable"));
}

Roe<void> CallMediaCoordinator::ResumeFromInbound() {
  CallDirectDriver* direct = DirectIfBuilt();
  return direct ? direct->ResumeMeshMediaFromInbound(call_id_) : Roe<void>(Error("direct media path unavailable"));
}

} // namespace pbr
