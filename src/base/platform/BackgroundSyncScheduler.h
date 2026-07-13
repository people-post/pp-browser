#pragma once

#include <cstdint>
#include <functional>

namespace pbr {

/**
 * Owns foreground/background relay poll cadence (P006).
 * Call Tick() from the UI loop; background in-process poll uses ResumeIO briefly.
 */
class BackgroundSyncScheduler {
public:
  using SyncFn = std::function<void(bool force)>;

  static BackgroundSyncScheduler& Instance();

  void SetSyncHandler(SyncFn handler);
  void Tick();
  /** FCM / WorkManager wake — force a sync even if interval not elapsed. */
  void RequestWakeSync();

private:
  SyncFn handler_;
  uint64_t last_poll_ms_ = 0;
  bool bg_io_held_ = false;
};

} // namespace pbr
