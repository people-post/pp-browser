#include "foundation/runtime/BackgroundActivity.h"

#include "common/Logger.h"
#include "common/PbrCompat.h"

#import <UIKit/UIKit.h>

#include <mutex>

namespace pbr {

struct BackgroundActivity::State {
  std::mutex mutex;
  UIBackgroundTaskIdentifier task = UIBackgroundTaskInvalid;
  std::string name;
};

namespace {

/** Ends the task once, whoever comes first: the owner going out of scope or the OS's expiration handler. */
void EndBackgroundTask(BackgroundActivity::State& state) {
  UIBackgroundTaskIdentifier task = UIBackgroundTaskInvalid;
  {
    std::lock_guard lock(state.mutex);
    task = state.task;
    state.task = UIBackgroundTaskInvalid;
  }
  if (task != UIBackgroundTaskInvalid) {
    // Both UIApplication calls are documented as safe off the main thread.
    [[UIApplication sharedApplication] endBackgroundTask:task];
  }
}

} // namespace

BackgroundActivity::BackgroundActivity(const std::string& name) : state_(std::make_shared<State>()) {
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
                                                           EndBackgroundTask(*state);
                                                         }];
  std::lock_guard lock(state_->mutex);
  state_->task = task;
}

BackgroundActivity::~BackgroundActivity() {
  if (state_) {
    EndBackgroundTask(*state_);
  }
}

} // namespace pbr
