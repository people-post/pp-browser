#pragma once

#include "common/Module.h"

#include <functional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

class CallSessionManager;

/** 1:1 call phase — sole management owner for ring/accept/media/listen sequencing. */
enum class CallPhase {
  Idle = 0,
  Ringing,
  Accepting,
  OutboundCalling,
  JoinedLocal,
  MediaPending,
  MediaConnecting,
  InCall,
  ConnectFailed,
};

enum class CallLifecycleEvent {
  InviteSeen = 0,
  InviteCleared,
  OutboundStarted,
  AcceptClicked,
  DeclineClicked,
  LeaveClicked,
  RetryClicked,
  AcceptSucceeded,
  AcceptFailed,
  DeclineDone,
  LeaveDone,
  MediaDeferred,
  MediaKeyReady,
  DirectConnected,
  ConnectFailedEvt,
  RemoteEnded,
};

const char* CallPhaseName(CallPhase phase);
const char* CallLifecycleEventName(CallLifecycleEvent ev);

/**
 * Orchestrates 1:1 call phases and thread policy. Controllers post clicks here;
 * session/media/listen subsystems report outcomes here. Never calls ListenOn or
 * encrypt on the caller thread.
 */
class CallLifecycle : public Module {
public:
  using ChromeRefreshFn = std::function<void()>;
  using ListenDesireFn = std::function<void(bool want)>;

  CallLifecycle() = default;

  void Bind(CallSessionManager* sessions);
  void ClearBinding();

  void SetOnChromeRefresh(ChromeRefreshFn fn);
  void SetOnListenDesireChanged(ListenDesireFn fn);

  CallPhase Phase() const { return phase_; }
  const std::string& ActiveCallId() const { return call_id_; }
  const std::string& AcceptingCallId() const { return accepting_call_id_; }
  const std::string& LastRingCallId() const { return last_ring_call_id_; }
  bool WantEphemeralListen() const { return want_ephemeral_listen_; }
  const std::string& LastError() const { return last_error_; }
  void ClearLastError() { last_error_.clear(); }

  /** True while Accept is in flight for this invite — chrome must not re-show ring. */
  bool ShouldSuppressRing(const std::string& call_id) const;

  void Apply(CallLifecycleEvent ev, const std::string& call_id = {});

  /** Resolve Accept click call_id from controller/shell fallbacks. */
  void NoteRingCallId(const std::string& call_id);

private:
  void SetPhase(CallPhase next, const std::string& call_id, CallLifecycleEvent ev);
  void UpdateListenDesire();
  void NotifyChrome();
  void PostAcceptInvite(const std::string& call_id);
  void PostDeclineInvite(const std::string& call_id);
  void PostLeaveCall(const std::string& call_id);
  void PostRetryMedia(const std::string& call_id);

  CallSessionManager* sessions_ = nullptr;
  CallPhase phase_ = CallPhase::Idle;
  std::string call_id_;
  std::string accepting_call_id_;
  std::string last_ring_call_id_;
  bool want_ephemeral_listen_ = false;
  std::string last_error_;

  ChromeRefreshFn on_chrome_refresh_;
  ListenDesireFn on_listen_desire_;
};

} // namespace pbr
