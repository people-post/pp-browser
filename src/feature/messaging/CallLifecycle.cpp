#include "feature/messaging/CallLifecycle.h"

#include "feature/messaging/CallSessionManager.h"
#include "base/runtime/AppRuntime.h"
#include "common/Logger.h"

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

void CallLifecycle::SetPhase(const CallPhase next, const std::string& call_id, const CallLifecycleEvent ev) {
  const CallPhase prev = phase_;
  phase_ = next;
  if (!call_id.empty()) {
    call_id_ = call_id;
  }
  if (next == CallPhase::Idle) {
    call_id_.clear();
    accepting_call_id_.clear();
  }
  log().info << "phase=" << CallPhaseName(prev) << "->" << CallPhaseName(next)
                << " event=" << CallLifecycleEventName(ev) << " call_id=" << call_id_;
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
  log().info << "WantEphemeralListen=" << (want ? 1 : 0) << " phase=" << CallPhaseName(phase_);
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
    logging::getLogger("CallLifecycle").info << "AcceptInvite worker enter call_id=" << call_id;
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
  AppRuntime::PostWorkerAndReplyOnUI<Roe<void>>(WorkerLane::Normal, 
      [sessions, call_id]() -> Roe<void> {
        if (!sessions) {
          return Error("Calls unavailable");
        }
        return sessions->DeclineInvite(call_id);
      },
      [this, call_id](Roe<void> declined) {
        if (!declined) {
          log().warning << "DeclineInvite failed call_id=" << call_id << " err=" << declined.error().message;
        }
        Apply(CallLifecycleEvent::DeclineDone, call_id);
      });
}

void CallLifecycle::PostLeaveCall(const std::string& call_id) {
  CallSessionManager* sessions = sessions_;
  // Critical: must not sit behind Normal work while Connect (also Critical) still dials —
  // StopLibp2pMedia aborts Connect via connect_generation_.
  AppRuntime::PostWorkerAndReplyOnUI<Roe<void>>(WorkerLane::Critical, 
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
  AppRuntime::PostWorkerAndReplyOnUI<Roe<void>>(WorkerLane::Normal, 
      [sessions, call_id]() -> Roe<void> {
        if (!sessions) {
          return Error("Calls unavailable");
        }
        return sessions->RetryP2pMedia(call_id);
      },
      [this, call_id](Roe<void> retried) {
        if (!retried) {
          log().warning << "RetryP2pMedia failed call_id=" << call_id << " err=" << retried.error().message;
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
    // Defer chrome refresh so the Accept click returns before ring teardown / DirtyWindow.
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
    SetPhase(CallPhase::JoinedLocal, call_id, ev);
    NotifyChrome();
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
