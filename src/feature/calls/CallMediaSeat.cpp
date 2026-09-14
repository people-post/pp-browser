#include "feature/calls/CallMediaSeat.h"

#include "foundation/runtime/AppRuntime.h"

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
      path_ = PathKind::None;
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
      path_ = PathKind::None;
    }
    // Epoch stays put — NoteStart/Acquire bump to invalidate in-flight Stop.
    log().info << "Release call_id=" << target << " epoch=" << epoch_at_post
               << " bound_now=" << bound_call_id_;
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
    log().info << "NoteStart bind call_id=" << call_id << " epoch=" << epoch_;
    return;
  }
  ++epoch_;
  if (path_ == PathKind::None) {
    path_ = PathKind::Direct;
  }
  log().info << "NoteStart call_id=" << call_id << " epoch=" << epoch_
             << " path=" << (path_ == PathKind::Hop ? "hop" : "direct");
}

void CallMediaSeat::NotePath(PathKind kind) {
  std::lock_guard lock(mu_);
  path_ = kind;
  log().info << "NotePath call_id=" << bound_call_id_
             << " path=" << (kind == PathKind::Hop ? "hop"
                                                   : kind == PathKind::Direct ? "direct" : "none");
}

bool CallMediaSeat::IsBound(const std::string& call_id) const {
  std::lock_guard lock(mu_);
  return !call_id.empty() && bound_call_id_ == call_id;
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

CallMediaSeat::Token CallMediaSeat::CurrentToken() const {
  std::lock_guard lock(mu_);
  Token t;
  t.epoch = epoch_;
  t.call_id = bound_call_id_;
  return t;
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
