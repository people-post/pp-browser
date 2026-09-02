#include "base/runtime/BackgroundSyncScheduler.h"

#include "common/chat/MessagingLimits.h"
#include "base/runtime/AppLifecycle.h"
#include "base/runtime/AppRuntime.h"
#include "common/Utilities.h"
#include "common/PbrCompat.h"

namespace pbr {

BackgroundSyncScheduler::BackgroundSyncScheduler() {
  redirectLogger("BackgroundSync");
}

BackgroundSyncScheduler& BackgroundSyncScheduler::Instance() {
  static BackgroundSyncScheduler instance;
  return instance;
}

void BackgroundSyncScheduler::SetSyncHandler(SyncFn handler) {
  handler_ = std::move(handler);
  EnsureRelayPollTimer();
  log().warning << "SetSyncHandler armed=" << (handler_ ? 1 : 0)
                << " timer_id=" << relay_poll_timer_id_;
}

void BackgroundSyncScheduler::EnsureRelayPollTimer() {
  if (!handler_) {
    StopRelayPollTimer();
    return;
  }
  if (relay_poll_timer_id_ != 0) {
    return;
  }
  const auto cadence = std::chrono::milliseconds(kForegroundRelayPollIntervalMs);
  relay_poll_timer_id_ = AppRuntime::ScheduleCoordinatorRepeating(cadence, [this]() {
    RunScheduledSync(false);
  });
  if (relay_poll_timer_id_ == 0) {
    log().error << "EnsureRelayPollTimer failed (ScheduleCoordinatorRepeating returned 0)";
  }
}

void BackgroundSyncScheduler::StopRelayPollTimer() {
  if (relay_poll_timer_id_ == 0) {
    return;
  }
  AppRuntime::CancelCoordinatorTimer(relay_poll_timer_id_);
  relay_poll_timer_id_ = 0;
}

void BackgroundSyncScheduler::RequestWakeSync() {
  if (!handler_) {
    return;
  }
  AppRuntime::PostCoordinatorCritical([this]() { RunScheduledSync(true); });
}

void BackgroundSyncScheduler::RequestCallWakeSync() {
  pending_call_wake_ = true;
  RequestWakeSync();
}

bool BackgroundSyncScheduler::ConsumeCallWake() {
  const bool was = pending_call_wake_;
  pending_call_wake_ = false;
  return was;
}

void BackgroundSyncScheduler::Tick() {
  // Relay poll cadence is driven by the coordinator timer wheel.
}

void BackgroundSyncScheduler::RunScheduledSync(bool force) {
  if (!handler_) {
    return;
  }
  if (AppLifecycle::IsForeground()) {
    bg_io_held_ = false;
  }
  if (!force) {
    const uint64_t now = util::NowUnixMs();
    const uint64_t interval = AppLifecycle::IsForeground() ? kForegroundRelayPollIntervalMs
                                                           : kBackgroundRelayPollIntervalMs;
    if (now - last_poll_ms_ < interval) {
      return;
    }
    last_poll_ms_ = now;
  } else {
    last_poll_ms_ = util::NowUnixMs();
  }
  if (!AppLifecycle::IsForeground()) {
    AppRuntime::ResumeBackgroundWork();
    bg_io_held_ = true;
  }
  handler_(force);
}

} // namespace pbr
