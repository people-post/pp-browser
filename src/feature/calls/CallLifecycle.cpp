#include "feature/calls/CallLifecycle.h"
#include "feature/calls/CallsThread.h"

#include "domain/messaging/CallLifecycleTransitionLogic.h"
#include "foundation/runtime/AppRuntime.h"
#include "common/Logger.h"
#include "common/PbrCompat.h"

#include <atomic>
#include <memory>

namespace pbr {

const char* CallPhaseName(const CallPhase phase) {
  switch (phase) {
  case CallPhase::Idle:
    return "Idle";
  case CallPhase::Ringing:
    return "Ringing";
  case CallPhase::Accepting:
    return "Accepting";
  case CallPhase::OutboundCalling:
    return "OutboundCalling";
  case CallPhase::JoinedLocal:
    return "JoinedLocal";
  case CallPhase::MediaPending:
    return "MediaPending";
  case CallPhase::MediaConnecting:
    return "MediaConnecting";
  case CallPhase::InCall:
    return "InCall";
  case CallPhase::ConnectFailed:
    return "ConnectFailed";
  }
  return "Unknown";
}

const char* CallMediaStatusName(const CallMediaStatus status) {
  switch (status) {
  case CallMediaStatus::None:
    return "None";
  case CallMediaStatus::Deciding:
    return "Deciding";
  case CallMediaStatus::DirectConnecting:
    return "DirectConnecting";
  case CallMediaStatus::HopWaiting:
    return "HopWaiting";
  case CallMediaStatus::HopAttaching:
    return "HopAttaching";
  case CallMediaStatus::DirectLive:
    return "DirectLive";
  case CallMediaStatus::HopLive:
    return "HopLive";
  case CallMediaStatus::Migrating:
    return "Migrating";
  case CallMediaStatus::DegradedTxOnly:
    return "DegradedTxOnly";
  case CallMediaStatus::Failed:
    return "Failed";
  case CallMediaStatus::Reconnecting:
    return "Reconnecting";
  }
  return "Unknown";
}

const char* CallArmedPlannerName(const CallArmedPlanner planner) {
  switch (planner) {
  case CallArmedPlanner::None:
    return "None";
  case CallArmedPlanner::Lifecycle:
    return "Lifecycle";
  case CallArmedPlanner::Bridge:
    return "Bridge";
  case CallArmedPlanner::Topology:
    return "Topology";
  }
  return "Unknown";
}

const char* CallLifecycleEventName(const CallLifecycleEvent ev) {
  switch (ev) {
  case CallLifecycleEvent::InviteSeen:
    return "InviteSeen";
  case CallLifecycleEvent::InviteCleared:
    return "InviteCleared";
  case CallLifecycleEvent::OutboundStarted:
    return "OutboundStarted";
  case CallLifecycleEvent::AcceptClicked:
    return "AcceptClicked";
  case CallLifecycleEvent::DeclineClicked:
    return "DeclineClicked";
  case CallLifecycleEvent::LeaveClicked:
    return "LeaveClicked";
  case CallLifecycleEvent::RetryClicked:
    return "RetryClicked";
  case CallLifecycleEvent::AcceptSucceeded:
    return "AcceptSucceeded";
  case CallLifecycleEvent::AcceptFailed:
    return "AcceptFailed";
  case CallLifecycleEvent::DeclineDone:
    return "DeclineDone";
  case CallLifecycleEvent::LeaveDone:
    return "LeaveDone";
  case CallLifecycleEvent::MediaDeferred:
    return "MediaDeferred";
  case CallLifecycleEvent::MediaKeyReady:
    return "MediaKeyReady";
  case CallLifecycleEvent::DirectConnected:
    return "DirectConnected";
  case CallLifecycleEvent::ConnectFailedEvt:
    return "ConnectFailed";
  case CallLifecycleEvent::RemoteEnded:
    return "RemoteEnded";
  case CallLifecycleEvent::PeerReconnected:
    return "PeerReconnected";
  }
  return "Unknown";
}

void CallLifecycle::BindSignalingPorts(CallLifecycleSignalingPorts ports) {
  ports_.Set(std::move(ports));
  // Logger::redirectTo is idempotent when already bound to the same name.
  redirectLogger("CallLifecycle");
}

void CallLifecycle::ClearBinding() {
  // Invalidate in-flight Accept/Decline/Leave UI replies before clearing ports.
  deferred_.Invalidate();
  ports_.Set({});
  phase_ = CallPhase::Idle;
  status_ = CallMediaStatus::None;
  media_cancel_gen_ = 0;
  call_id_.clear();
  accepting_call_id_.clear();
  want_ephemeral_listen_ = false;
  on_chrome_refresh_ = nullptr;
  on_listen_desire_ = nullptr;
  UpdateListenDesire();
}

void CallLifecycle::SetOnChromeRefresh(ChromeRefreshFn fn) {
  on_chrome_refresh_ = std::move(fn);
}

void CallLifecycle::SetOnListenDesireChanged(ListenDesireFn fn) {
  on_listen_desire_ = std::move(fn);
}

CallArmedPlanner CallLifecycle::ArmedPlanner() const {
  switch (status_) {
  case CallMediaStatus::None:
    return CallArmedPlanner::None;
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
  case CallMediaStatus::Failed:
    return CallArmedPlanner::None;
  }
  return CallArmedPlanner::None;
}

bool CallLifecycle::AllowsDirectPath() const {
  return status_ == CallMediaStatus::DirectConnecting ||
         status_ == CallMediaStatus::DegradedTxOnly ||
         status_ == CallMediaStatus::DirectLive || status_ == CallMediaStatus::Reconnecting;
}

bool CallLifecycle::AllowsHopPath() const {
  return status_ == CallMediaStatus::HopWaiting || status_ == CallMediaStatus::HopAttaching ||
         status_ == CallMediaStatus::HopLive || status_ == CallMediaStatus::Migrating;
}

bool CallLifecycle::MediaChromeLive() const {
  return phase_ == CallPhase::InCall &&
         (status_ == CallMediaStatus::DirectLive || status_ == CallMediaStatus::HopLive);
}

uint64_t CallLifecycle::BumpMediaCancelGen() {
  return ++media_cancel_gen_;
}

void CallLifecycle::SetStatusInternal(const CallMediaStatus next, const std::string& call_id,
                                      const char* reason) {
  const CallMediaStatus prev = status_;
  if (next == CallMediaStatus::Deciding && prev != CallMediaStatus::Deciding) {
    BumpMediaCancelGen();
  }
  status_ = next;
  if (!call_id.empty()) {
    call_id_ = call_id;
  }
  if (next == CallMediaStatus::DirectLive || next == CallMediaStatus::HopLive) {
    if (phase_ != CallPhase::InCall && phase_ != CallPhase::Idle) {
      const CallPhase prev_phase = phase_;
      phase_ = CallPhase::InCall;
      log().info << "phase=" << CallPhaseName(prev_phase) << "->InCall"
                 << " status=" << CallMediaStatusName(prev) << "->" << CallMediaStatusName(next)
                 << " reason=" << (reason ? reason : "") << " call_id=" << call_id_
                 << " cancel_gen=" << media_cancel_gen_
                 << " armed=" << CallArmedPlannerName(ArmedPlanner());
      UpdateListenDesire();
      return;
    }
  }
  if (next == CallMediaStatus::Failed && phase_ != CallPhase::Idle) {
    const CallPhase prev_phase = phase_;
    phase_ = CallPhase::ConnectFailed;
    log().info << "phase=" << CallPhaseName(prev_phase) << "->ConnectFailed"
               << " status=" << CallMediaStatusName(prev) << "->Failed"
               << " reason=" << (reason ? reason : "") << " call_id=" << call_id_
               << " cancel_gen=" << media_cancel_gen_;
    UpdateListenDesire();
    return;
  }
  log().info << "status=" << CallMediaStatusName(prev) << "->" << CallMediaStatusName(next)
             << " phase=" << CallPhaseName(phase_) << " reason=" << (reason ? reason : "")
             << " call_id=" << call_id_ << " cancel_gen=" << media_cancel_gen_
             << " armed=" << CallArmedPlannerName(ArmedPlanner());
}

void CallLifecycle::SetMediaStatus(const CallMediaStatus status, const std::string& call_id) {
  SetStatusInternal(status, call_id, "SetMediaStatus");
  NotifyChrome();
}

bool CallLifecycle::ShouldSuppressRing(const std::string& call_id) const {
  if (call_id.empty()) {
    return false;
  }
  if (!accepting_call_id_.empty() && accepting_call_id_ == call_id) {
    return true;
  }
  if (phase_ == CallPhase::Accepting && call_id_ == call_id) {
    return true;
  }
  return false;
}

void CallLifecycle::NoteRingCallId(const std::string& call_id) {
  if (!call_id.empty()) {
    last_ring_call_id_ = call_id;
  }
}

void CallLifecycle::SetPhase(const CallPhase next, const std::string& call_id,
                             const CallLifecycleEvent ev) {
  const CallPhase prev = phase_;
  const CallMediaStatus prev_status = status_;
  phase_ = next;
  if (!call_id.empty()) {
    call_id_ = call_id;
  }
  if (next == CallPhase::Idle) {
    call_id_.clear();
    accepting_call_id_.clear();
    status_ = CallMediaStatus::None;
    BumpMediaCancelGen();
  } else if (next == CallPhase::Ringing || next == CallPhase::Accepting) {
    status_ = CallMediaStatus::None;
  } else if (next == CallPhase::JoinedLocal && status_ == CallMediaStatus::None) {
    // Accept path will Deciding → Direct/Hop; start as Deciding so gates see Lifecycle armed.
    SetStatusInternal(CallMediaStatus::Deciding, call_id_, "AcceptJoined");
  } else if (next == CallPhase::MediaConnecting &&
             (status_ == CallMediaStatus::None || status_ == CallMediaStatus::Deciding)) {
    // Key ready / retry without an explicit path yet — prefer Direct until SoftMigrate.
    SetStatusInternal(CallMediaStatus::DirectConnecting, call_id_, "MediaConnecting");
  } else if (next == CallPhase::ConnectFailed) {
    status_ = CallMediaStatus::Failed;
  } else if (next == CallPhase::InCall &&
             (status_ == CallMediaStatus::None || status_ == CallMediaStatus::Deciding ||
              status_ == CallMediaStatus::DirectConnecting || status_ == CallMediaStatus::Reconnecting ||
              status_ == CallMediaStatus::HopAttaching || status_ == CallMediaStatus::HopWaiting)) {
    // Legacy DirectConnected without prior SetMediaStatus — assume direct Live.
    if (status_ != CallMediaStatus::HopAttaching && status_ != CallMediaStatus::HopWaiting &&
        status_ != CallMediaStatus::HopLive && status_ != CallMediaStatus::Migrating) {
      status_ = CallMediaStatus::DirectLive;
    } else if (status_ == CallMediaStatus::HopAttaching || status_ == CallMediaStatus::HopWaiting) {
      status_ = CallMediaStatus::HopLive;
    }
  }
  log().info << "phase=" << CallPhaseName(prev) << "->" << CallPhaseName(next)
             << " status=" << CallMediaStatusName(prev_status) << "->" << CallMediaStatusName(status_)
             << " event=" << CallLifecycleEventName(ev) << " call_id=" << call_id_
             << " cancel_gen=" << media_cancel_gen_
             << " armed=" << CallArmedPlannerName(ArmedPlanner());
  UpdateListenDesire();
}

void CallLifecycle::UpdateListenDesire() {
  const bool want = phase_ == CallPhase::Ringing || phase_ == CallPhase::Accepting ||
                    phase_ == CallPhase::OutboundCalling || phase_ == CallPhase::JoinedLocal ||
                    phase_ == CallPhase::MediaPending || phase_ == CallPhase::MediaConnecting ||
                    phase_ == CallPhase::InCall || phase_ == CallPhase::ConnectFailed;
  if (want == want_ephemeral_listen_) {
    return;
  }
  want_ephemeral_listen_ = want;
  log().info << "WantEphemeralListen=" << (want ? 1 : 0) << " phase=" << CallPhaseName(phase_)
             << " status=" << CallMediaStatusName(status_);
  if (on_listen_desire_) {
    on_listen_desire_(want);
  }
}

void CallLifecycle::NotifyChrome() {
  // GUI boundary: the chrome callback runs on UI whichever thread owns the lifecycle.
  if (!on_chrome_refresh_) {
    return;
  }
  if (AppRuntime::CurrentlyOnUI()) {
    on_chrome_refresh_();
    return;
  }
  const auto guard = deferred_.token();
  const uint64_t epoch = deferred_.Snapshot();
  AppRuntime::PostUI([refresh = on_chrome_refresh_, guard, epoch]() {
    if (DeferredSelf::Alive(guard, epoch)) {
      refresh();
    }
  });
}

void CallLifecycle::PostAcceptInvite(const std::string& call_id) {
  const auto signaling = ports_.Get();
  auto accept = signaling->accept_invite;
  const auto guard = deferred_.token();
  const uint64_t epoch = deferred_.Snapshot();
  AppRuntime::ResumeBackgroundWork();  // relay-fallback sends run on workers
  // Accept runs on the calls owner (thread-ownership t2a) — never behind PollInbox, never blocking:
  // it awaits the circuit park asynchronously and reports once.
  CallsThread::Post([this, accept = std::move(accept), call_id, guard, epoch]() {
    if (!DeferredSelf::Alive(guard, epoch)) {
      return;
    }
    log().info << "AcceptInvite owner enter call_id=" << call_id;
    auto reply = [this, call_id, guard, epoch](Roe<void> accepted) {
      CallsThread::Post([this, call_id, accepted = std::move(accepted), guard, epoch]() {
        if (DeferredSelf::Alive(guard, epoch)) {
          OnAcceptResult(call_id, accepted);
        }
      });
    };
    if (!accept) {
      reply(Error("Calls unavailable"));
      return;
    }
    accept(call_id, std::move(reply));
  });
}

void CallLifecycle::OnAcceptResult(const std::string& call_id, const Roe<void>& accepted) {
  // B-CONFLICT: Accept B while AcceptInvite(A) still runs — drop A's late UI result so it
  // cannot JoinedLocal/RemoteEnded-clobber B (or Idle after LeaveCallIfActiveExcept).
  if (accepting_call_id_ != call_id && call_id_ != call_id) {
    log().info << "AcceptInvite result ignored stale call_id=" << call_id << " active=" << call_id_
               << " accepting=" << accepting_call_id_;
    return;
  }
  if (!accepted) {
    log().warning << "AcceptInvite failed call_id=" << call_id << " err=" << accepted.error().message;
    last_error_ = accepted.error().message;
    Apply(CallLifecycleEvent::AcceptFailed, call_id);
    return;
  }
  log().info << "AcceptInvite ok call_id=" << call_id;
  Apply(CallLifecycleEvent::AcceptSucceeded, call_id);
}

void CallLifecycle::PostOnOwnerAndReply(std::function<Roe<void>()> work, std::function<void(Roe<void>)> reply) {
  const auto guard = deferred_.token();
  const uint64_t epoch = deferred_.Snapshot();
  CallsThread::Post([work = std::move(work), reply = std::move(reply), guard,
                                                              epoch]() {
    Roe<void> result = work();
    CallsThread::Post([reply, result = std::move(result), guard, epoch]() {
      if (DeferredSelf::Alive(guard, epoch)) {
        reply(result);
      }
    });
  });
}

void CallLifecycle::PostDeclineInvite(const std::string& call_id) {
  const auto signaling = ports_.Get();
  auto decline = signaling->decline_invite;
  PostOnOwnerAndReply(
      [decline = std::move(decline), call_id]() -> Roe<void> {
        return decline ? decline(call_id) : Roe<void>(Error("Calls unavailable"));
      },
      [this, call_id](Roe<void> declined) {
        if (!declined) {
          log().warning << "DeclineInvite failed call_id=" << call_id << " err=" << declined.error().message;
        }
        Apply(CallLifecycleEvent::DeclineDone, call_id);
      });
}

void CallLifecycle::PostLeaveCall(const std::string& call_id) {
  const auto signaling = ports_.Get();
  auto leave = signaling->leave_call;
  // On the calls owner, which never blocks — Leave cannot sit behind a dialing Connect (bridge) or
  // an Accept waiting for its circuit park.
  PostOnOwnerAndReply(
      [leave = std::move(leave), call_id]() -> Roe<void> {
        return leave ? leave(call_id) : Roe<void>(Error("Calls unavailable"));
      },
      [this, call_id](Roe<void> left) {
        if (!left) {
          log().warning << "LeaveCall failed call_id=" << call_id << " err=" << left.error().message;
        }
        Apply(CallLifecycleEvent::LeaveDone, call_id);
      });
}

void CallLifecycle::PostRetryMedia(const std::string& call_id) {
  PostRestartMedia(call_id, ports_.Get()->retry_p2p_media, CallLifecycleEvent::RetryClicked);
}

void CallLifecycle::PostResumeMedia(const std::string& call_id) {
  PostRestartMedia(call_id, ports_.Get()->resume_p2p_media, CallLifecycleEvent::PeerReconnected);
}

void CallLifecycle::PostRestartMedia(const std::string& call_id,
                                     std::function<Roe<void>(const std::string&)> restart,
                                     const CallLifecycleEvent ev) {
  const auto guard = deferred_.token();
  const uint64_t epoch = deferred_.Snapshot();
  // Re-arm Direct before the restart → BeginSession (Failed Status blocks AllowsDirectPath).
  SetMediaStatus(CallMediaStatus::DirectConnecting, call_id);
  // Calls owner, not a worker: the restart runs the engine (StartSfu / Stop — capture threads) and
  // the bridge's connect sequence, owner-only like every other media start. Posted, not inline,
  // so it never re-enters Apply.
  CallsThread::Post([this, restart = std::move(restart), call_id, guard, epoch, ev]() {
    if (!DeferredSelf::Alive(guard, epoch)) {
      return;
    }
    const Roe<void> restarted = restart ? restart(call_id) : Roe<void>(Error("Calls unavailable"));
    if (!restarted) {
      log().warning << CallLifecycleEventName(ev) << " media restart failed call_id=" << call_id
                    << " err=" << restarted.error().message;
      Apply(CallLifecycleEvent::ConnectFailedEvt, call_id);
      return;
    }
    // A resume over the peer's live stream commits Connected inside the restart — never move an
    // InCall call back to Connecting.
    if (phase_ == CallPhase::ConnectFailed) {
      SetPhase(CallPhase::MediaConnecting, call_id, ev);
    }
    NotifyChrome();
  });
}

void CallLifecycle::Apply(const CallLifecycleEvent ev, const std::string& call_id_arg) {
  const auto signaling = ports_.Get();
  CallLifecycleTransitionContext ctx;
  ctx.phase = phase_;
  ctx.status = status_;
  ctx.active_call_id = call_id_;
  ctx.accepting_call_id = accepting_call_id_;
  ctx.last_ring_call_id = last_ring_call_id_;
  ctx.event_call_id = call_id_arg;
  ctx.sessions_bound = signaling->IsBound();
  ctx.allows_direct_path = AllowsDirectPath();

  const CallLifecycleTransitionOutcome out = DecideCallLifecycleTransition(ev, ctx);
  const CallLifecycleAction actions = out.actions;
  if (actions == CallLifecycleAction::None) {
    return;
  }

  if (HasAction(actions, CallLifecycleAction::LogIgnored)) {
    if (out.ignore_reason) {
      log().info << out.ignore_reason
                 << (out.call_id.empty() ? "" : (" call_id=" + out.call_id));
    }
    // "already in flight" still notifies chrome; pure ignore returns after log.
    if (!HasAction(actions, CallLifecycleAction::NotifyChrome) &&
        !HasAction(actions, CallLifecycleAction::DeferChrome)) {
      return;
    }
  }

  if (HasAction(actions, CallLifecycleAction::NoteRing)) {
    NoteRingCallId(out.call_id);
  }
  if (HasAction(actions, CallLifecycleAction::NoteRemoteEnded)) {
    remote_ended_call_id_ = call_id_arg.empty() ? call_id_ : call_id_arg;
  }
  if (HasAction(actions, CallLifecycleAction::SetAccepting)) {
    accepting_call_id_ = out.call_id;
    last_ring_call_id_ = out.call_id;
  }
  if (HasAction(actions, CallLifecycleAction::ClearAccepting)) {
    accepting_call_id_.clear();
  }
  if (HasAction(actions, CallLifecycleAction::SetPhase)) {
    SetPhase(out.next_phase, out.call_id, ev);
  } else if (HasAction(actions, CallLifecycleAction::LogKeepPhase)) {
    log().info << "AcceptSucceeded keep phase=" << CallPhaseName(phase_)
               << " status=" << CallMediaStatusName(status_) << " call_id=" << call_id_;
  }
  if (HasAction(actions, CallLifecycleAction::SetStatus)) {
    SetStatusInternal(out.next_status, out.call_id,
                      out.status_reason ? out.status_reason : CallLifecycleEventName(ev));
  }

  if (HasAction(actions, CallLifecycleAction::PostAcceptInvite)) {
    log().info << "PostAcceptInvite queued call_id=" << out.call_id;
    PostAcceptInvite(out.call_id);
  }
  if (HasAction(actions, CallLifecycleAction::PostDeclineInvite)) {
    PostDeclineInvite(out.call_id);
  }
  if (HasAction(actions, CallLifecycleAction::PostLeaveCall)) {
    PostLeaveCall(out.call_id);
  }
  if (HasAction(actions, CallLifecycleAction::PostRetryMedia)) {
    PostRetryMedia(out.call_id);
  }
  if (HasAction(actions, CallLifecycleAction::PostResumeMedia)) {
    PostResumeMedia(out.call_id);
  }

  if (HasAction(actions, CallLifecycleAction::KickAnswererDirectMedia)) {
    const std::string kick_id = call_id_.empty() ? out.call_id : call_id_;
    auto kick = signaling->kick_answerer_direct_media;
    auto media_active = signaling->media_active_for_call;
    if (kick && AllowsDirectPath()) {
      log().info << "AcceptSucceeded KickAnswererDirectMedia StartSfu arm call_id=" << kick_id
                 << " status=" << CallMediaStatusName(status_);
      kick(kick_id);
      const auto guard = deferred_.token();
      const uint64_t epoch = deferred_.Snapshot();
      CallsThread::Post([this, kick_id, kick, media_active, guard, epoch]() {
        if (!DeferredSelf::Alive(guard, epoch)) {
          return;
        }
        const auto signaling = ports_.Get();
        if (!signaling->IsBound() || call_id_ != kick_id || !AllowsDirectPath()) {
          return;
        }
        if (media_active && media_active(kick_id)) {
          return;
        }
        log().info << "AcceptSucceeded retry KickAnswererDirectMedia StartSfu call_id="
                   << kick_id << " status=" << CallMediaStatusName(status_);
        if (kick) {
          kick(kick_id);
        }
      });
    } else {
      log().info << "AcceptSucceeded skip KickAnswerer StartSfu call_id=" << kick_id
                 << " ports=" << (signaling->IsBound() ? 1 : 0)
                 << " status=" << CallMediaStatusName(status_);
    }
  }

  if (HasAction(actions, CallLifecycleAction::DeferChrome)) {
    if (CallsThread::IsCurrent()) {
      const auto guard = deferred_.token();
      const uint64_t epoch = deferred_.Snapshot();
      CallsThread::Post([this, guard, epoch]() {
        if (!DeferredSelf::Alive(guard, epoch)) {
          return;
        }
        NotifyChrome();
      });
    } else {
      NotifyChrome();
    }
  } else if (HasAction(actions, CallLifecycleAction::NotifyChrome)) {
    NotifyChrome();
  }
}

} // namespace pbr
