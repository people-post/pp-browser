#pragma once

#include "common/Module.h"

#include <cstdint>
#include <functional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

class CallSessionManager;

/**
 * Call chrome / shell State (V037). JoinedLocal / MediaPending / MediaConnecting are
 * Calling-like for planner arming until a future rename.
 */
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

/**
 * Media Status under Calling-like / InCall (V037). Arms at most one planner.
 */
enum class CallMediaStatus {
  None = 0,
  Deciding,
  DirectConnecting,
  HopWaiting,
  HopAttaching,
  DirectLive,
  HopLive,
  Migrating,
  DegradedTxOnly,
  Failed,
};

enum class CallArmedPlanner {
  None = 0,
  Lifecycle,
  Bridge,
  Topology,
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
const char* CallMediaStatusName(CallMediaStatus status);
const char* CallArmedPlannerName(CallArmedPlanner planner);
const char* CallLifecycleEventName(CallLifecycleEvent ev);

/**
 * Orchestrates call State + media Status (V037). Controllers post clicks here;
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
  CallMediaStatus Status() const { return status_; }
  CallArmedPlanner ArmedPlanner() const;
  uint64_t MediaCancelGen() const { return media_cancel_gen_; }

  /** Bridge may ScheduleStart / BeginSession. */
  bool AllowsDirectPath() const;
  /** Topology may SoftMigrate / inbound attach / hop StartSfu. */
  bool AllowsHopPath() const;
  /** Chrome Connected: InCall + DirectLive|HopLive. */
  bool MediaChromeLive() const;

  /**
   * Set media Status (and bump cancel gen when entering Deciding).
   * Live statuses also advance phase to InCall.
   */
  void SetMediaStatus(CallMediaStatus status, const std::string& call_id = {});
  /** Bump cancel gen so late hop/direct workers abort StartSfu. */
  uint64_t BumpMediaCancelGen();

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
  void SetStatusInternal(CallMediaStatus next, const std::string& call_id, const char* reason);
  void UpdateListenDesire();
  void NotifyChrome();
  void PostAcceptInvite(const std::string& call_id);
  void PostDeclineInvite(const std::string& call_id);
  void PostLeaveCall(const std::string& call_id);
  void PostRetryMedia(const std::string& call_id);

  CallSessionManager* sessions_ = nullptr;
  CallPhase phase_ = CallPhase::Idle;
  CallMediaStatus status_ = CallMediaStatus::None;
  uint64_t media_cancel_gen_ = 0;
  std::string call_id_;
  std::string accepting_call_id_;
  std::string last_ring_call_id_;
  bool want_ephemeral_listen_ = false;
  std::string last_error_;

  ChromeRefreshFn on_chrome_refresh_;
  ListenDesireFn on_listen_desire_;
};

} // namespace pbr
