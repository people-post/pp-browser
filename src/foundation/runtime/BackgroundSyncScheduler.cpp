#include "foundation/runtime/BackgroundSyncScheduler.h"

#include "common/chat/MessagingLimits.h"
#include "foundation/runtime/AppLifecycle.h"
#include "foundation/runtime/AppRuntime.h"
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
  // A push wakes a backgrounded app: its work is paused, the coordinator included, so the sync
  // posted below would only run once the user opened the app. Resume it for a window, long
  // enough to fetch the inbox and ring, then pause again: in the background a phone is reached by
  // push only (power).
  if (!AppLifecycle::IsForeground()) {
    AppLifecycle::OpenWakeWindow(std::chrono::milliseconds(kBackgroundWakeWindowMs));
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
  if (!force) {
    // In the background a phone is reached by push only (P010): no poll of its own, even inside a
    // wake window (the wake's own sync fetched the inbox) — unless a call is on (its signaling may
    // come through the relay) or it stays connected (P011).
    const bool foreground = AppLifecycle::IsForeground();
    const bool call = AppLifecycle::CallInProgress();
    if (!foreground && !call && !AppLifecycle::StaysConnected()) {
      return;
    }
    const uint64_t now = util::NowUnixMs();
    const uint64_t interval =
        foreground || call ? kForegroundRelayPollIntervalMs : kStayConnectedRelayPollIntervalMs;
    if (now - last_poll_ms_ < interval) {
      return;
    }
    last_poll_ms_ = now;
  } else {
    last_poll_ms_ = util::NowUnixMs();
  }
  handler_(force);
}

} // namespace pbr
