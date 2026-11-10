#include "lib/calls/CallMediaSeat.h"

namespace pbr {
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
      Token out;
      out.epoch = epoch_;
      out.call_id = bound_call_id_;
      return out;
    }
    if (!bound_call_id_.empty()) {
      release_other = bound_call_id_;
      bound_call_id_.clear();
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
    ClearAttachLocked();
    out.epoch = epoch_;
    out.call_id = call_id;
    log().info << "Acquire call_id=" << call_id << " epoch=" << epoch_;
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
      ClearAttachLocked();
    } else if (attach_call_id_ == target) {
      ClearAttachLocked();
    }
    // Epoch stays put — NoteStart/Acquire bump to invalidate in-flight Stop.
    log().info << "Release call_id=" << target << " epoch=" << epoch_at_post << " bound_now=" << bound_call_id_;
    // B24: a stale Leave/Ended for an *older* call (relay inbox replays them on every poll)
    // must not stop the engine that a live, different call is holding.
    if (!bound_call_id_.empty() && bound_call_id_ != target && attach_call_id_ != target) {
      log().info << "Release ignored: seat held by another call bound=" << bound_call_id_ << " release=" << target;
      return;
    }
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
      log().info << "Release skip stale token call_id=" << token.call_id << " token_epoch=" << token.epoch
                 << " seat_epoch=" << epoch_;
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
  ++epoch_;
  if (bound_call_id_ != call_id) {
    bound_call_id_ = call_id;
    log().info << "NoteStart bind call_id=" << call_id << " epoch=" << epoch_;
    return;
  }
  log().info << "NoteStart call_id=" << call_id << " epoch=" << epoch_;
}

CallMediaSeat::AttachBeginResult CallMediaSeat::BeginAttach(const std::string& call_id, const std::string& hop_peer_id,
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
  log().info << "BeginAttach call_id=" << call_id << " hop=" << hop_peer_id << " gen=" << attach_gen_;
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
  log().info << "EndAttach call_id=" << ticket.call_id << " hop=" << ticket.hop_peer_id << " gen=" << ticket.gen;
  ClearAttachLocked();
}

void CallMediaSeat::EndAttachIfMatching(const std::string& call_id, const std::string& hop_peer_id) {
  std::lock_guard lock(mu_);
  if (call_id.empty() || attach_call_id_ != call_id || attach_hop_peer_id_ != hop_peer_id) {
    return;
  }
  log().info << "EndAttachIfMatching call_id=" << call_id << " hop=" << hop_peer_id << " gen=" << attach_gen_;
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

bool CallMediaSeat::MatchesToken(const Token& token) const {
  std::lock_guard lock(mu_);
  return !token.call_id.empty() && token.call_id == bound_call_id_ && token.epoch == epoch_;
}

bool CallMediaSeat::AllowsPathOp(const Token& token) const {
  std::lock_guard lock(mu_);
  return !token.call_id.empty() && token.call_id == bound_call_id_;
}

std::string CallMediaSeat::BoundCallId() const {
  std::lock_guard lock(mu_);
  return bound_call_id_;
}

uint64_t CallMediaSeat::Epoch() const {
  std::lock_guard lock(mu_);
  return epoch_;
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
  // Passive: the seat's callers run on the calls owner, so the stop runs here, before any start
  // that follows it in the same step.
  stop(call_id, epoch_at_post, force);
}

} // namespace pbr
