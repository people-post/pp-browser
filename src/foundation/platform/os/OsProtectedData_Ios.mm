#include "foundation/platform/os/OsProtectedData.h"

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>

#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <sys/sysctl.h>
#include <uuid/uuid.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace pbr::os {

namespace {

std::mutex g_mutex;
std::map<ProtectedDataHandlerId, std::function<void()>> g_handlers;
ProtectedDataHandlerId g_next_id = 1;
std::once_flag g_observer_once;

void RunHandlers() {
  std::vector<std::function<void()>> handlers;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    handlers.reserve(g_handlers.size());
    for (const auto& entry : g_handlers) {
      handlers.push_back(entry.second);
    }
  }
  for (const auto& fn : handlers) {
    if (fn) {
      fn();
    }
  }
}

// Last known protectedDataAvailable, for callers off the main thread (UIApplication is main-thread only). Kept up to
// date by the two UIKit notifications and by every main-thread read; until the first of those it says "available",
// and the Keychain read that follows is what decides (it answers Unavailable while the device is locked).
std::atomic<bool> g_available{true};

void InstallObservers() {
  std::call_once(g_observer_once, [] {
    // Never removed: one pair for the process, the registry above decides who hears about it.
    // queue nil → the blocks run on the posting thread (UIKit posts these on the main thread).
    NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
    [center addObserverForName:UIApplicationProtectedDataDidBecomeAvailable
                        object:nil
                         queue:nil
                    usingBlock:^(NSNotification* /*note*/) {
                      g_available.store(true);
                      RunHandlers();
                    }];
    [center addObserverForName:UIApplicationProtectedDataWillBecomeUnavailable
                        object:nil
                         queue:nil
                    usingBlock:^(NSNotification* /*note*/) {
                      g_available.store(false);
                    }];
  });
}

bool ReadOnMainThread() {
  const bool available = [UIApplication sharedApplication].protectedDataAvailable;
  g_available.store(available);
  return available;
}

} // namespace

bool ProtectedDataAvailable() {
  InstallObservers();
  if ([NSThread isMainThread]) {
    return ReadOnMainThread();
  }
  // Never block on the main thread (it may be waiting for this one): answer from the cache, refresh it there.
  dispatch_async(dispatch_get_main_queue(), ^{
    (void)ReadOnMainThread();
  });
  return g_available.load();
}

ProtectedDataHandlerId OnProtectedDataAvailable(std::function<void()> fn) {
  InstallObservers();
  std::lock_guard<std::mutex> lock(g_mutex);
  const ProtectedDataHandlerId id = g_next_id++;
  g_handlers.emplace(id, std::move(fn));
  return id;
}

void RemoveProtectedDataHandler(const ProtectedDataHandlerId id) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_handlers.erase(id);
}

GraceClock GraceClockNow() {
  GraceClock now;
  // kern.bootsessionuuid: a new UUID each boot. All zero if it cannot be read (then every boot looks alike,
  // and the monotonic clock, which restarts at boot, still closes old windows in most cases).
  char text[64] = {};
  size_t length = sizeof(text) - 1;
  if (sysctlbyname("kern.bootsessionuuid", text, &length, nullptr, 0) == 0) {
    uuid_t parsed;
    if (uuid_parse(text, parsed) == 0) {
      std::copy(parsed, parsed + sizeof(uuid_t), now.boot_id.begin());
    }
  }
  // mach_continuous_time keeps counting while the device sleeps (mach_absolute_time does not).
  mach_timebase_info_data_t timebase{};
  if (mach_timebase_info(&timebase) == KERN_SUCCESS && timebase.denom != 0) {
    const unsigned __int128 nanos =
        static_cast<unsigned __int128>(mach_continuous_time()) * timebase.numer / timebase.denom;
    now.mono_seconds = static_cast<int64_t>(nanos / 1000000000u);
  }
  return now;
}

} // namespace pbr::os
