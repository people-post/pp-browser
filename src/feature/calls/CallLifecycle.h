#pragma once

#include "common/Module.h"
#include "domain/messaging/CallLifecycleTypes.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

class CallSessionManager;

/**
 * Orchestrates call State + media Status (V037). Controllers post clicks here;
 * session/media/listen subsystems report outcomes here. Never calls ListenOn or
 * encrypt on the caller thread.
 *
 * Transitions: pure `DecideCallLifecycleTransition` (domain); this class executes actions.
 */
class CallLifecycle : public Module {
public:
  using ChromeRefreshFn = std::function<void()>;
  using ListenDesireFn = std::function<void(bool want)>;

  CallLifecycle() = default;
  ~CallLifecycle() override {
    ClearBinding();
  }

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
  /**
   * Shared so worker/UI lambdas can detect ClearBinding / destroy without touching
   * a dangling `this` (stale DeclineInvite/LeaveCall replies across tests).
   */
  std::shared_ptr<std::atomic<uint64_t>> async_epoch_ =
      std::make_shared<std::atomic<uint64_t>>(0);
  std::string call_id_;
  std::string accepting_call_id_;
  std::string last_ring_call_id_;
  bool want_ephemeral_listen_ = false;
  std::string last_error_;

  ChromeRefreshFn on_chrome_refresh_;
  ListenDesireFn on_listen_desire_;
};

} // namespace pbr
