#include "feature/calls/CallLifecycle.h"

#include "feature/calls/CallSessionManager.h"
#include "foundation/runtime/AppRuntime.h"
#include "common/Logger.h"
#include "common/PbrCompat.h"

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
  }
  return "Unknown";
}

void CallLifecycle::Bind(CallSessionManager* sessions) {
  sessions_ = sessions;
  // Logger::redirectTo is idempotent when already bound to the same name.
  redirectLogger("CallLifecycle");
}

void CallLifecycle::ClearBinding() {
  sessions_ = nullptr;
  phase_ = CallPhase::Idle;
  status_ = CallMediaStatus::None;
  media_cancel_gen_ = 0;
  call_id_.clear();
  accepting_call_id_.clear();
  want_ephemeral_listen_ = false;
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
         status_ == CallMediaStatus::DirectLive;
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
              status_ == CallMediaStatus::DirectConnecting ||
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
  if (!on_chrome_refresh_) {
    return;
  }
  if (AppRuntime::CurrentlyOnUI()) {
    on_chrome_refresh_();
    return;
  }
  AppRuntime::PostUI([this]() {
    if (on_chrome_refresh_) {
      on_chrome_refresh_();
    }
  });
}

void CallLifecycle::PostAcceptInvite(const std::string& call_id) {
  CallSessionManager* sessions = sessions_;
  AppRuntime::ResumeBackgroundWork();
  // Never Browser IO — AcceptInvite was starved behind PollInbox on Samsung (queued, no IO enter).
  // Same escape hatch as offerer Connect worker / call-control MediaKey send.
  AppRuntime::PostWorkerCritical([this, sessions, call_id]() {
    log().info << "AcceptInvite worker enter call_id=" << call_id;
    Roe<void> accepted = Error("Calls unavailable");
    if (sessions) {
      accepted = sessions->AcceptInvite(call_id);
    }
    AppRuntime::PostUI([this, call_id, accepted = std::move(accepted)]() mutable {
      if (!accepted) {
        log().warning << "AcceptInvite failed call_id=" << call_id << " err=" << accepted.error().message;
        last_error_ = accepted.error().message;
        Apply(CallLifecycleEvent::AcceptFailed, call_id);
        return;
      }
      log().info << "AcceptInvite ok call_id=" << call_id;
      Apply(CallLifecycleEvent::AcceptSucceeded, call_id);
    });
  });
}

void CallLifecycle::PostDeclineInvite(const std::string& call_id) {
  CallSessionManager* sessions = sessions_;
  AppRuntime::PostWorkerAndReplyOnUI<Roe<void>>(
      WorkerLane::Normal,
      [sessions, call_id]() -> Roe<void> {
        if (!sessions) {
          return Error("Calls unavailable");
        }
        return sessions->DeclineInvite(call_id);
      },
      [this, call_id](Roe<void> declined) {
        if (!declined) {
          log().warning << "DeclineInvite failed call_id=" << call_id
                        << " err=" << declined.error().message;
        }
        Apply(CallLifecycleEvent::DeclineDone, call_id);
      });
}

void CallLifecycle::PostLeaveCall(const std::string& call_id) {
  CallSessionManager* sessions = sessions_;
  // Critical: must not sit behind Normal work while Connect (also Critical) still dials —
  // StopMeshMedia aborts Connect via connect_generation_.
  AppRuntime::PostWorkerAndReplyOnUI<Roe<void>>(
      WorkerLane::Critical,
      [sessions, call_id]() -> Roe<void> {
        if (!sessions) {
          return Error("Calls unavailable");
        }
        return sessions->LeaveCall(call_id);
      },
      [this, call_id](Roe<void> left) {
        if (!left) {
          log().warning << "LeaveCall failed call_id=" << call_id << " err=" << left.error().message;
        }
        Apply(CallLifecycleEvent::LeaveDone, call_id);
      });
}

void CallLifecycle::PostRetryMedia(const std::string& call_id) {
  CallSessionManager* sessions = sessions_;
  AppRuntime::PostWorkerAndReplyOnUI<Roe<void>>(
      WorkerLane::Normal,
      [sessions, call_id]() -> Roe<void> {
        if (!sessions) {
          return Error("Calls unavailable");
        }
        return sessions->RetryP2pMedia(call_id);
      },
      [this, call_id](Roe<void> retried) {
        if (!retried) {
          log().warning << "RetryP2pMedia failed call_id=" << call_id
                        << " err=" << retried.error().message;
          Apply(CallLifecycleEvent::ConnectFailedEvt, call_id);
          return;
        }
        SetPhase(CallPhase::MediaConnecting, call_id, CallLifecycleEvent::RetryClicked);
        NotifyChrome();
      });
}

void CallLifecycle::Apply(const CallLifecycleEvent ev, const std::string& call_id_arg) {
  std::string call_id = call_id_arg;
  if (call_id.empty()) {
    call_id = call_id_;
  }

  switch (ev) {
  case CallLifecycleEvent::InviteSeen:
    if (phase_ == CallPhase::Idle || phase_ == CallPhase::Ringing) {
      NoteRingCallId(call_id);
      SetPhase(CallPhase::Ringing, call_id, ev);
      NotifyChrome();
    } else if (phase_ == CallPhase::OutboundCalling || phase_ == CallPhase::InCall ||
               phase_ == CallPhase::MediaConnecting || phase_ == CallPhase::JoinedLocal ||
               phase_ == CallPhase::MediaPending || phase_ == CallPhase::ConnectFailed) {
      NoteRingCallId(call_id);
      NotifyChrome();
    }
    break;

  case CallLifecycleEvent::InviteCleared:
    if (phase_ == CallPhase::Ringing) {
      SetPhase(CallPhase::Idle, {}, ev);
      NotifyChrome();
    }
    break;

  case CallLifecycleEvent::OutboundStarted:
    SetPhase(CallPhase::OutboundCalling, call_id, ev);
    SetStatusInternal(CallMediaStatus::Deciding, call_id, "OutboundStarted");
    NotifyChrome();
    break;

  case CallLifecycleEvent::AcceptClicked: {
    if (call_id.empty()) {
      call_id = last_ring_call_id_;
    }
    if (call_id.empty()) {
      log().info << "AcceptClicked ignored (no call_id)";
      return;
    }
    if (!accepting_call_id_.empty() && accepting_call_id_ == call_id) {
      log().info << "AcceptClicked already in flight call_id=" << call_id;
      NotifyChrome();
      return;
    }
    accepting_call_id_ = call_id;
    last_ring_call_id_ = call_id;
    SetPhase(CallPhase::Accepting, call_id, ev);
    // Queue AcceptInvite on IO before chrome refresh — NotifyChrome/RefreshPendingRing
    // must not gate session work (Samsung: mDNS advertise lock hung UI before this ran).
    log().info << "PostAcceptInvite queued call_id=" << call_id;
    PostAcceptInvite(call_id);
    // Defer chrome refresh so the Accept click returns before ring teardown / DirtyCallChrome.
    if (AppRuntime::CurrentlyOnUI()) {
      AppRuntime::PostUI([this]() { NotifyChrome(); });
    } else {
      NotifyChrome();
    }
    break;
  }

  case CallLifecycleEvent::DeclineClicked:
    if (call_id.empty()) {
      call_id = last_ring_call_id_;
    }
    if (call_id.empty()) {
      log().info << "DeclineClicked ignored (no call_id)";
      return;
    }
    SetPhase(CallPhase::Idle, {}, ev);
    NotifyChrome();
    PostDeclineInvite(call_id);
    break;

  case CallLifecycleEvent::LeaveClicked:
    if (call_id.empty()) {
      log().info << "LeaveClicked ignored (no call_id)";
      return;
    }
    SetPhase(CallPhase::Idle, {}, ev);
    NotifyChrome();
    PostLeaveCall(call_id);
    break;

  case CallLifecycleEvent::RetryClicked:
    if (call_id.empty() || phase_ != CallPhase::ConnectFailed) {
      log().info << "RetryClicked ignored phase=" << CallPhaseName(phase_);
      return;
    }
    PostRetryMedia(call_id);
    break;

  case CallLifecycleEvent::AcceptSucceeded:
    accepting_call_id_.clear();
    // Do not clobber MediaPending/MediaConnecting if answerer ScheduleStart already
    // deferred (MediaDeferred) or keyed (MediaKeyReady) on the UI queue ahead of us.
    if (phase_ != CallPhase::MediaPending && phase_ != CallPhase::MediaConnecting &&
        phase_ != CallPhase::InCall) {
      SetPhase(CallPhase::JoinedLocal, call_id, ev);
    } else {
      log().info << "AcceptSucceeded keep phase=" << CallPhaseName(phase_)
                 << " status=" << CallMediaStatusName(status_) << " call_id=" << call_id_;
    }
    NotifyChrome();
    // Answerer media must start on UI after Status is visible (worker ScheduleStart alone
    // can PostUI before AcceptSucceeded and silently no-op if Status/session race).
    // Log tokens include StartSfu so dogfood filters that omit CallSessionManager still see it.
    {
      const std::string kick_id = call_id_.empty() ? call_id : call_id_;
      if (sessions_ && AllowsDirectPath()) {
        log().info << "AcceptSucceeded KickAnswererDirectMedia StartSfu arm call_id=" << kick_id
                   << " status=" << CallMediaStatusName(status_);
        sessions_->KickAnswererDirectMediaIfArmed(kick_id);
        // UI-queue retry (not coordinator — PauseBackgroundWork can drop one-shots).
        AppRuntime::PostUI([this, kick_id]() {
          if (!sessions_ || call_id_ != kick_id || !AllowsDirectPath()) {
            return;
          }
          const auto& media = sessions_->Media();
          if (media.IsActive() && media.ActiveCallId() == kick_id) {
            const auto snap = media.HealthSnapshot();
            if (media.IsConnected() || snap.tx_audio_frames > 0) {
              return;
            }
          }
          log().info << "AcceptSucceeded retry KickAnswererDirectMedia StartSfu call_id="
                     << kick_id << " status=" << CallMediaStatusName(status_);
          sessions_->KickAnswererDirectMediaIfArmed(kick_id);
        });
        AppRuntime::PostUI([this, kick_id]() {
          AppRuntime::PostUI([this, kick_id]() {
            if (!sessions_ || call_id_ != kick_id || !AllowsDirectPath()) {
              return;
            }
            const auto& media = sessions_->Media();
            if (media.IsActive() && media.ActiveCallId() == kick_id) {
              const auto snap = media.HealthSnapshot();
              if (media.IsConnected() || snap.tx_audio_frames > 0) {
                return;
              }
            }
            log().info << "AcceptSucceeded late retry KickAnswererDirectMedia StartSfu call_id="
                       << kick_id << " status=" << CallMediaStatusName(status_);
            sessions_->KickAnswererDirectMediaIfArmed(kick_id);
          });
        });
      } else {
        log().info << "AcceptSucceeded skip KickAnswerer StartSfu call_id=" << kick_id
                   << " sessions=" << (sessions_ ? 1 : 0)
                   << " status=" << CallMediaStatusName(status_);
      }
    }
    break;

  case CallLifecycleEvent::AcceptFailed:
    accepting_call_id_.clear();
    SetPhase(CallPhase::Ringing, call_id, ev);
    NotifyChrome();
    break;

  case CallLifecycleEvent::DeclineDone:
  case CallLifecycleEvent::LeaveDone:
  case CallLifecycleEvent::RemoteEnded:
    accepting_call_id_.clear();
    SetPhase(CallPhase::Idle, {}, ev);
    NotifyChrome();
    break;

  case CallLifecycleEvent::MediaDeferred:
    if (phase_ == CallPhase::Accepting || phase_ == CallPhase::JoinedLocal ||
        phase_ == CallPhase::OutboundCalling || phase_ == CallPhase::MediaConnecting) {
      SetPhase(CallPhase::MediaPending, call_id, ev);
      NotifyChrome();
    }
    break;

  case CallLifecycleEvent::MediaKeyReady:
    if (phase_ == CallPhase::MediaPending || phase_ == CallPhase::JoinedLocal ||
        phase_ == CallPhase::Accepting) {
      SetPhase(CallPhase::MediaConnecting, call_id, ev);
      NotifyChrome();
    }
    break;

  case CallLifecycleEvent::DirectConnected:
    // Prefer existing Live/path Status; otherwise DirectLive.
    if (status_ == CallMediaStatus::HopAttaching || status_ == CallMediaStatus::HopWaiting ||
        status_ == CallMediaStatus::HopLive || status_ == CallMediaStatus::Migrating) {
      SetStatusInternal(CallMediaStatus::HopLive, call_id, "DirectConnected");
    } else if (status_ != CallMediaStatus::DirectLive && status_ != CallMediaStatus::HopLive) {
      SetStatusInternal(CallMediaStatus::DirectLive, call_id, "DirectConnected");
    }
    SetPhase(CallPhase::InCall, call_id, ev);
    NotifyChrome();
    break;

  case CallLifecycleEvent::ConnectFailedEvt:
    if (phase_ != CallPhase::Idle) {
      SetPhase(CallPhase::ConnectFailed, call_id, ev);
      NotifyChrome();
    }
    break;
  }
}

} // namespace pbr
