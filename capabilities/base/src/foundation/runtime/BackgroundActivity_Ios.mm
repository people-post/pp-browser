#include "foundation/runtime/BackgroundActivity.h"

#include "common/Logger.h"
#include "common/PbrCompat.h"

#import <UIKit/UIKit.h>

#include <mutex>

namespace pbr {

struct BackgroundActivity::State {
  std::mutex mutex;
  UIBackgroundTaskIdentifier task = UIBackgroundTaskInvalid;
  /** The OS's time ran out (possibly before `task` was stored: the handler may run first). */
  bool expired = false;
  bool ended = false;
  std::string name;
};

namespace {

/** Ends the task exactly once, whoever comes first: the owner, the expiration handler, or a late begin. */
void EndBackgroundTask(BackgroundActivity::State& state) {
  UIBackgroundTaskIdentifier task = UIBackgroundTaskInvalid;
  {
    std::lock_guard lock(state.mutex);
    if (state.ended || state.task == UIBackgroundTaskInvalid) {
      return;
    }
    task = state.task;
    state.ended = true;
  }
  // Both UIApplication calls are documented as safe off the main thread.
  [[UIApplication sharedApplication] endBackgroundTask:task];
}

} // namespace

// No ARC in this repo, and a transfer's worker thread has no autorelease pool of its own: each body has one.
BackgroundActivity::BackgroundActivity(const std::string& name) : state_(std::make_shared<State>()) {
  @autoreleasepool {
    state_->name = name;
    UIApplication* app = [UIApplication sharedApplication];
    if (app == nil) {
      return;
    }
    std::weak_ptr<State> weak = state_;
    NSString* task_name = [NSString stringWithUTF8String:name.c_str()];
    const UIBackgroundTaskIdentifier task = [app beginBackgroundTaskWithName:task_name
                                                           expirationHandler:^{
                                                             const std::shared_ptr<State> state = weak.lock();
                                                             if (!state) {
                                                               return;
                                                             }
                                                             logging::getLogger("BackgroundActivity").info
                                                                 << "background time ran out name=" << state->name;
                                                             {
                                                               std::lock_guard lock(state->mutex);
                                                               state->expired = true;
                                                             }
                                                             EndBackgroundTask(*state);
                                                           }];
    if (task == UIBackgroundTaskInvalid) {
      return;
    }
    bool expired_already = false;
    {
      std::lock_guard lock(state_->mutex);
      state_->task = task;
      expired_already = state_->expired;
    }
    if (expired_already) {
      EndBackgroundTask(*state_); // the handler ran before the id was known: end it now
    }
  }
}

BackgroundActivity::~BackgroundActivity() {
  @autoreleasepool {
    if (state_) {
      EndBackgroundTask(*state_);
    }
  }
}

} // namespace pbr
