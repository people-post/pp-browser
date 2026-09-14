#include "feature/calls/CallMediaSeat.h"

#include "foundation/runtime/AppRuntime.h"

namespace pbr {
namespace {

const char* MediaStateName(CallMediaSeat::MediaState s) {
  switch (s) {
  case CallMediaSeat::MediaState::Idle:
    return "Idle";
  case CallMediaSeat::MediaState::Connecting:
    return "Connecting";
  case CallMediaSeat::MediaState::Live:
    return "Live";
  case CallMediaSeat::MediaState::Failed:
    return "Failed";
  }
  return "?";
}

} // namespace

CallMediaSeat::CallMediaSeat() {
  redirectLogger("CallMediaSeat");
}

void CallMediaSeat::SetTeardownHooks(TopologyStoppedFn topology_stopped, StopEngineFn stop_engine) {
  std::lock_guard lock(mu_);
  topology_stopped_ = std::move(topology_stopped);
  stop_engine_ = std::move(stop_engine);
}

CallMediaSeat::Token CallMediaSeat::Acquire(const std::string& call_id) {
  if (call_id.empty()) {
    return {};
  }
  std::string release_other;
  {
    std::lock_guard lock(mu_);
    if (bound_call_id_ == call_id) {
      if (state_ == MediaState::Idle || state_ == MediaState::Failed) {
        state_ = MediaState::Connecting;
      }
      Token out;
      out.epoch = epoch_;
      out.call_id = bound_call_id_;
      return out;
    }
    if (!bound_call_id_.empty()) {
      release_other = bound_call_id_;
      bound_call_id_.clear();
      path_ = PathKind::None;
      state_ = MediaState::Idle;
      ClearAttachLocked();
    }
  }
  // Prior bind: force teardown before bumping epoch (awaited/ordered). Do not use the
  // epoch-guarded async path — that would race NoteStart on the new call.
  if (!release_other.empty()) {
    log().info << "Acquire releasing prior call_id=" << release_other << " for=" << call_id;
    InvokeTeardown(release_other, /*epoch_at_post=*/0, /*force=*/true);
  }

  Token out;
  {
    std::lock_guard lock(mu_);
    ++epoch_;
    bound_call_id_ = call_id;
    path_ = PathKind::None;
    state_ = MediaState::Connecting;
    ClearAttachLocked();
    out.epoch = epoch_;
    out.call_id = call_id;
    log().info << "Acquire call_id=" << call_id << " epoch=" << epoch_
               << " media=" << MediaStateName(state_);
  }
  return out;
}

void CallMediaSeat::Release(const std::string& call_id) {
  uint64_t epoch_at_post = 0;
  std::string target = call_id;
  {
    std::lock_guard lock(mu_);
    if (target.empty()) {
      target = bound_call_id_;
    }
    epoch_at_post = epoch_;
    if (target.empty() || bound_call_id_ == target) {
      bound_call_id_.clear();
      path_ = PathKind::None;
      state_ = MediaState::Idle;
      ClearAttachLocked();
    } else if (attach_call_id_ == target) {
      ClearAttachLocked();
    }
    // Epoch stays put — NoteStart/Acquire bump to invalidate in-flight Stop.
    log().info << "Release call_id=" << target << " epoch=" << epoch_at_post
               << " bound_now=" << bound_call_id_ << " media=" << MediaStateName(state_);
  }
  InvokeTeardown(target, epoch_at_post, /*force=*/false);
}

void CallMediaSeat::Release(const Token& token) {
  if (token.call_id.empty()) {
    return;
  }
  {
    std::lock_guard lock(mu_);
    if (token.epoch != epoch_) {
      log().info << "Release skip stale token call_id=" << token.call_id
                 << " token_epoch=" << token.epoch << " seat_epoch=" << epoch_;
      return;
    }
  }
  Release(token.call_id);
}

void CallMediaSeat::NoteStart(const std::string& call_id) {
  if (call_id.empty()) {
    return;
  }
  std::lock_guard lock(mu_);
  if (bound_call_id_ != call_id) {
    ++epoch_;
    bound_call_id_ = call_id;
    if (path_ == PathKind::None) {
      path_ = PathKind::Direct;
    }
    // StartSfu alone is not duplex Live — chrome waits for NoteLive.
    if (state_ != MediaState::Live) {
      state_ = MediaState::Connecting;
    }
    log().info << "NoteStart bind call_id=" << call_id << " epoch=" << epoch_
               << " media=" << MediaStateName(state_);
    return;
  }
  ++epoch_;
  if (path_ == PathKind::None) {
    path_ = PathKind::Direct;
  }
  if (state_ != MediaState::Live) {
    state_ = MediaState::Connecting;
  }
  log().info << "NoteStart call_id=" << call_id << " epoch=" << epoch_
             << " path=" << (path_ == PathKind::Hop ? "hop" : "direct")
             << " media=" << MediaStateName(state_);
}

void CallMediaSeat::NotePath(PathKind kind) {
  std::lock_guard lock(mu_);
  path_ = kind;
  log().info << "NotePath call_id=" << bound_call_id_
             << " path=" << (kind == PathKind::Hop ? "hop"
                                                   : kind == PathKind::Direct ? "direct" : "none");
}

void CallMediaSeat::NoteConnecting(const std::string& call_id) {
  if (call_id.empty()) {
    return;
  }
  std::lock_guard lock(mu_);
  if (bound_call_id_ != call_id) {
    return;
  }
  // SoftMigrate / reattach: stay Live so chrome uses health for reconnecting, not demote.
  if (state_ == MediaState::Live) {
    return;
  }
  state_ = MediaState::Connecting;
  log().info << "NoteConnecting call_id=" << call_id << " media=" << MediaStateName(state_);
}

void CallMediaSeat::NoteLive(const std::string& call_id) {
  if (call_id.empty()) {
    return;
  }
  std::lock_guard lock(mu_);
  if (bound_call_id_ != call_id) {
    log().info << "NoteLive ignored (not bound) call_id=" << call_id
               << " bound=" << bound_call_id_;
    return;
  }
  if (state_ == MediaState::Live) {
    return;
  }
  state_ = MediaState::Live;
  log().info << "NoteLive call_id=" << call_id << " epoch=" << epoch_
             << " path=" << (path_ == PathKind::Hop ? "hop" : "direct");
}

void CallMediaSeat::NoteFailed(const std::string& call_id) {
  if (call_id.empty()) {
    return;
  }
  std::lock_guard lock(mu_);
  if (bound_call_id_ != call_id) {
    return;
  }
  state_ = MediaState::Failed;
  ClearAttachLocked();
  log().info << "NoteFailed call_id=" << call_id;
}

CallMediaSeat::AttachBeginResult CallMediaSeat::BeginAttach(const std::string& call_id,
                                                            const std::string& hop_peer_id,
                                                            AttachTicket* out) {
  if (call_id.empty() || hop_peer_id.empty() || !out) {
    return AttachBeginResult::Rejected;
  }
  std::lock_guard lock(mu_);
  if (!attach_call_id_.empty()) {
    if (attach_call_id_ == call_id && attach_hop_peer_id_ == hop_peer_id) {
      out->gen = attach_gen_;
      out->call_id = attach_call_id_;
      out->hop_peer_id = attach_hop_peer_id_;
      log().info << "BeginAttach coalesce same hop call_id=" << call_id << " hop=" << hop_peer_id
                 << " gen=" << attach_gen_;
      return AttachBeginResult::CoalescedSameHop;
    }
    log().info << "BeginAttach defer other hop call_id=" << call_id << " hop=" << hop_peer_id
               << " in_flight_hop=" << attach_hop_peer_id_ << " gen=" << attach_gen_;
    return AttachBeginResult::DeferredOtherHop;
  }
  ++attach_gen_;
  attach_call_id_ = call_id;
  attach_hop_peer_id_ = hop_peer_id;
  out->gen = attach_gen_;
  out->call_id = call_id;
  out->hop_peer_id = hop_peer_id;
  if (bound_call_id_ == call_id && state_ != MediaState::Live) {
    state_ = MediaState::Connecting;
  }
  log().info << "BeginAttach call_id=" << call_id << " hop=" << hop_peer_id
             << " gen=" << attach_gen_;
  return AttachBeginResult::Started;
}

bool CallMediaSeat::IsAttachCurrent(const AttachTicket& ticket) const {
  std::lock_guard lock(mu_);
  return ticket.gen != 0 && ticket.gen == attach_gen_ && ticket.call_id == attach_call_id_ &&
         ticket.hop_peer_id == attach_hop_peer_id_;
}

void CallMediaSeat::EndAttach(const AttachTicket& ticket) {
  std::lock_guard lock(mu_);
  if (ticket.gen == 0 || ticket.gen != attach_gen_) {
    return;
  }
  if (ticket.call_id != attach_call_id_ || ticket.hop_peer_id != attach_hop_peer_id_) {
    return;
  }
  log().info << "EndAttach call_id=" << ticket.call_id << " hop=" << ticket.hop_peer_id
             << " gen=" << ticket.gen;
  ClearAttachLocked();
}

void CallMediaSeat::EndAttachIfMatching(const std::string& call_id, const std::string& hop_peer_id) {
  std::lock_guard lock(mu_);
  if (call_id.empty() || attach_call_id_ != call_id || attach_hop_peer_id_ != hop_peer_id) {
    return;
  }
  log().info << "EndAttachIfMatching call_id=" << call_id << " hop=" << hop_peer_id
             << " gen=" << attach_gen_;
  ClearAttachLocked();
}

void CallMediaSeat::CancelAttachForCall(const std::string& call_id) {
  std::lock_guard lock(mu_);
  if (call_id.empty() || attach_call_id_ != call_id) {
    return;
  }
  log().info << "CancelAttach call_id=" << call_id << " gen=" << attach_gen_;
  ClearAttachLocked();
}

bool CallMediaSeat::HasAttachInFlight() const {
  std::lock_guard lock(mu_);
  return !attach_call_id_.empty();
}

std::string CallMediaSeat::AttachingHopPeerId() const {
  std::lock_guard lock(mu_);
  return attach_hop_peer_id_;
}

bool CallMediaSeat::IsBound(const std::string& call_id) const {
  std::lock_guard lock(mu_);
  return !call_id.empty() && bound_call_id_ == call_id;
}

bool CallMediaSeat::IsLive(const std::string& call_id) const {
  std::lock_guard lock(mu_);
  return !call_id.empty() && bound_call_id_ == call_id && state_ == MediaState::Live;
}

std::string CallMediaSeat::BoundCallId() const {
  std::lock_guard lock(mu_);
  return bound_call_id_;
}

uint64_t CallMediaSeat::Epoch() const {
  std::lock_guard lock(mu_);
  return epoch_;
}

CallMediaSeat::PathKind CallMediaSeat::Path() const {
  std::lock_guard lock(mu_);
  return path_;
}

CallMediaSeat::MediaState CallMediaSeat::State() const {
  std::lock_guard lock(mu_);
  return state_;
}

CallMediaSeat::Token CallMediaSeat::CurrentToken() const {
  std::lock_guard lock(mu_);
  Token t;
  t.epoch = epoch_;
  t.call_id = bound_call_id_;
  return t;
}

void CallMediaSeat::ClearAttachLocked() {
  attach_call_id_.clear();
  attach_hop_peer_id_.clear();
  // Keep attach_gen_ monotonic so stale EndAttach no-ops.
}

void CallMediaSeat::InvokeTeardown(const std::string& call_id, uint64_t epoch_at_post, bool force) {
  TopologyStoppedFn topo;
  StopEngineFn stop;
  {
    std::lock_guard lock(mu_);
    topo = topology_stopped_;
    stop = stop_engine_;
  }
  if (topo) {
    topo(call_id);
  }
  if (!stop) {
    return;
  }
  auto run = [stop = std::move(stop), call_id, epoch_at_post, force]() {
    stop(call_id, epoch_at_post, force);
  };
  if (force || AppRuntime::CurrentlyOnUI()) {
    // Force (Acquire prior) and UI-thread Release run Stop inline so StartSfu cannot race.
    if (AppRuntime::CurrentlyOnUI()) {
      run();
    } else if (force) {
      // Ordered: front of UI queue before any scheduled StartSfu for the new call.
      AppRuntime::PostUIFront(std::move(run));
    } else {
      AppRuntime::PostUIFront(std::move(run));
    }
  } else {
    AppRuntime::PostUIFront(std::move(run));
  }
}

} // namespace pbr
