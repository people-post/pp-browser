#pragma once

#include "common/Error.h"
#include "feature/calls/SharedPorts.h"
#include "common/Module.h"
#include "domain/messaging/CallLifecycleTypes.h"
#include "foundation/runtime/DeferredSelf.h"

#include <cstdint>
#include <functional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Stack-filled signaling façade for CallLifecycle (V041).
 * Lifecycle must not hold CallSessionManager* — workers copy these functions.
 */
struct CallLifecycleSignalingPorts {
  /** Async: `done` runs once, on the calls owner. */
  std::function<void(const std::string& call_id, std::function<void(Roe<void>)> done)> accept_invite;
  std::function<Roe<void>(const std::string& call_id)> decline_invite;
  std::function<Roe<void>(const std::string& call_id)> leave_call;
  std::function<Roe<void>(const std::string& call_id)> retry_p2p_media;
  /** Restart media for a failed, open call, keeping the peer's inbound stream (PeerReconnected). */
  std::function<Roe<void>(const std::string& call_id)> resume_p2p_media;
  std::function<void(const std::string& call_id)> kick_answerer_direct_media;
  /** True when call media engine is active for this call_id. */
  std::function<bool(const std::string& call_id)> media_active_for_call;

  bool IsBound() const { return static_cast<bool>(accept_invite); }
};

/**
 * Orchestrates call State + media Status (V037). Controllers post clicks here;
 * session/media/listen subsystems report outcomes here. Never calls ListenOn or
 * encrypt on the caller thread.
 *
 * Transitions: pure `DecideCallLifecycleTransition` (domain); this class executes actions.
 * Signaling I/O via CallLifecycleSignalingPorts from CallStack (V041) — no CallSessionManager*.
 */
class CallLifecycle : public Module {
public:
  using ChromeRefreshFn = std::function<void()>;
  using ListenDesireFn = std::function<void(bool want)>;

  CallLifecycle() = default;
  ~CallLifecycle() override {
    ClearBinding();
  }

  void BindSignalingPorts(CallLifecycleSignalingPorts ports);
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
  void OnAcceptResult(const std::string& call_id, const Roe<void>& accepted);
  /** Run `work` on the calls owner, then `reply` on UI (dropped after ClearBinding). */
  void PostOnOwnerAndReply(std::function<Roe<void>()> work, std::function<void(Roe<void>)> reply);
  void PostDeclineInvite(const std::string& call_id);
  void PostLeaveCall(const std::string& call_id);
  void PostRetryMedia(const std::string& call_id);
  void PostResumeMedia(const std::string& call_id);
  /** Re-arm Direct and run `restart` (retry / resume) on the calls owner; MediaConnecting or Failed. */
  void PostRestartMedia(const std::string& call_id, std::function<Roe<void>(const std::string&)> restart,
                        CallLifecycleEvent ev);

  SharedPorts<CallLifecycleSignalingPorts> ports_;
  CallPhase phase_ = CallPhase::Idle;
  CallMediaStatus status_ = CallMediaStatus::None;
  uint64_t media_cancel_gen_ = 0;
  /**
   * DeferredSelf so worker/UI lambdas detect ClearBinding / destroy without touching
   * a dangling `this` (stale DeclineInvite/LeaveCall replies across tests).
   */
  DeferredSelf deferred_;
  std::string call_id_;
  std::string accepting_call_id_;
  std::string last_ring_call_id_;
  bool want_ephemeral_listen_ = false;
  std::string last_error_;

  ChromeRefreshFn on_chrome_refresh_;
  ListenDesireFn on_listen_desire_;
};

} // namespace pbr
