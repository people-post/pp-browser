#pragma once

#include "common/Module.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include "common/PbrCompat.h"

namespace pbr {

/** The relay poll's pace in the foreground (and during a live session, e.g. a call). */
inline constexpr uint64_t kForegroundRelayPollIntervalMs = 2000;
/**
 * In the background a phone is reached by push only (power): its work is paused (AppLifecycle) and
 * the relay poll with it. A push / WorkManager wake resumes the work for this long — enough to fetch
 * the inbox and ring — then it pauses again.
 */
inline constexpr uint64_t kBackgroundWakeWindowMs = 20000;
/** P011 — staying connected in the background (opt-in, Android): the relay poll's pace there. */
inline constexpr uint64_t kStayConnectedRelayPollIntervalMs = 30000;

/**
 * Owns the relay poll (foreground only) and the wakes (P010).
 * Poll runs on the coordinator timer wheel; wake paths post immediate coordinator messages. In the
 * background the app's work is paused and it is reached by push only: a push / WorkManager wake
 * resumes it for kBackgroundWakeWindowMs.
 */
class BackgroundSyncScheduler : public Module {
public:
  using SyncFn = std::function<void(bool force)>;

  static BackgroundSyncScheduler& Instance();

  void SetSyncHandler(SyncFn handler);
  /** Legacy no-op — relay poll is coordinator-driven. */
  void Tick();
  /**
   * FCM / WorkManager wake — force a sync even if interval not elapsed. In the background the
   * app's work resumes for kBackgroundWakeWindowMs, then pauses again (push only, for power).
   */
  void RequestWakeSync();
  /** An urgent wake (a call's push, V006) — same sync path; ConsumeUrgentWake() is true once after. */
  void RequestUrgentWakeSync();
  /** True if the most recent forced wake was urgent; clears the flag. */
  bool ConsumeUrgentWake();

private:
  BackgroundSyncScheduler();

  void EnsureRelayPollTimer();
  void StopRelayPollTimer();
  void RunScheduledSync(bool force);

  SyncFn handler_;
  uint64_t relay_poll_timer_id_ = 0;
  uint64_t last_poll_ms_ = 0;
  bool pending_urgent_wake_ = false;
};

} // namespace pbr
