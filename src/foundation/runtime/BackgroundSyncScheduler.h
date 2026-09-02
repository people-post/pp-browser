#pragma once

#include "common/Module.h"

#include <cstdint>
#include <functional>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Owns foreground/background relay poll cadence (P006).
 * Poll runs on the coordinator timer wheel; wake paths post immediate coordinator messages.
 */
class BackgroundSyncScheduler : public Module {
public:
  using SyncFn = std::function<void(bool force)>;

  static BackgroundSyncScheduler& Instance();

  void SetSyncHandler(SyncFn handler);
  /** Legacy no-op — relay poll is coordinator-driven. */
  void Tick();
  /** FCM / WorkManager wake — force a sync even if interval not elapsed. */
  void RequestWakeSync();
  /** Opaque call_wake (V006) — same sync path; ConsumeCallWake() is true once after. */
  void RequestCallWakeSync();
  /** True if the most recent forced wake was call_wake; clears the flag. */
  bool ConsumeCallWake();

private:
  BackgroundSyncScheduler();

  void EnsureRelayPollTimer();
  void StopRelayPollTimer();
  void RunScheduledSync(bool force);

  SyncFn handler_;
  uint64_t relay_poll_timer_id_ = 0;
  uint64_t last_poll_ms_ = 0;
  bool bg_io_held_ = false;
  bool pending_call_wake_ = false;
};

} // namespace pbr
