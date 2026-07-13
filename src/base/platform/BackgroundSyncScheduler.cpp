#include "base/platform/BackgroundSyncScheduler.h"

#include "base/messaging/MessagingLimits.h"
#include "base/platform/AppLifecycle.h"
#include "base/platform/BrowserThread.h"
#include "common/Utilities.h"

namespace pbr {

BackgroundSyncScheduler& BackgroundSyncScheduler::Instance() {
  static BackgroundSyncScheduler instance;
  return instance;
}

void BackgroundSyncScheduler::SetSyncHandler(SyncFn handler) {
  handler_ = std::move(handler);
}

void BackgroundSyncScheduler::RequestWakeSync() {
  if (!handler_) {
    return;
  }
  if (!AppLifecycle::IsForeground()) {
    BrowserThread::ResumeIO();
    bg_io_held_ = true;
  }
  handler_(true);
  last_poll_ms_ = util::NowUnixMs();
}

void BackgroundSyncScheduler::Tick() {
  if (!handler_) {
    return;
  }
  if (AppLifecycle::IsForeground()) {
    bg_io_held_ = false;
  }
  const uint64_t now = util::NowUnixMs();
  const uint64_t interval =
      AppLifecycle::IsForeground() ? kForegroundRelayPollIntervalMs : kBackgroundRelayPollIntervalMs;
  if (now - last_poll_ms_ < interval) {
    return;
  }
  if (!AppLifecycle::IsForeground()) {
    BrowserThread::ResumeIO();
    bg_io_held_ = true;
  }
  handler_(false);
  last_poll_ms_ = now;
}

} // namespace pbr
