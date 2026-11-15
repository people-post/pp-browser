#include "foundation/platform/PlatformLifecycleEvents.h"

#include "foundation/runtime/AppLifecycle.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <utility>
#include <vector>

namespace pbr {

namespace {

struct Listener {
  PlatformLifecycleEvents::ListenerId id = 0;
  std::function<void()> fn;
};

// Added / removed from any thread (owners register as they come up); called on the UI thread.
std::mutex g_mu;
std::vector<Listener> g_entered_background;
std::vector<Listener> g_will_enter_foreground;
std::vector<Listener> g_did_enter_foreground;
PlatformLifecycleEvents::ListenerId g_next_id = 1;

PlatformLifecycleEvents::ListenerId Add(std::vector<Listener>& listeners, std::function<void()> fn) {
  std::lock_guard lock(g_mu);
  const auto id = g_next_id++;
  listeners.push_back({id, std::move(fn)});
  return id;
}

/** A copy taken under the lock: listeners run outside it (one may add or remove another). */
std::vector<std::function<void()>> Snapshot(const std::vector<Listener>& listeners) {
  std::lock_guard lock(g_mu);
  std::vector<std::function<void()>> out;
  out.reserve(listeners.size());
  for (const Listener& listener : listeners) {
    out.push_back(listener.fn);
  }
  return out;
}

} // namespace

PlatformLifecycleEvents::ListenerId PlatformLifecycleEvents::AddEnteredBackgroundListener(
    std::function<void()> listener) {
  return Add(g_entered_background, std::move(listener));
}

PlatformLifecycleEvents::ListenerId PlatformLifecycleEvents::AddWillEnterForegroundListener(
    std::function<void()> listener) {
  return Add(g_will_enter_foreground, std::move(listener));
}

PlatformLifecycleEvents::ListenerId PlatformLifecycleEvents::AddDidEnterForegroundListener(
    std::function<void()> listener) {
  return Add(g_did_enter_foreground, std::move(listener));
}

void PlatformLifecycleEvents::RemoveListener(const ListenerId id) {
  if (id == 0) {
    return;
  }
  std::lock_guard lock(g_mu);
  for (auto* listeners : {&g_entered_background, &g_will_enter_foreground, &g_did_enter_foreground}) {
    std::erase_if(*listeners, [id](const Listener& listener) { return listener.id == id; });
  }
}

void PlatformLifecycleEvents::OnDidEnterBackground() {
  AppLifecycle::logger().info << "Did enter background mono_ms=" << MonoMs();
  for (const auto& listener : Snapshot(g_entered_background)) {
    listener();
  }
}

void PlatformLifecycleEvents::OnWillEnterForeground() {
  AppLifecycle::logger().info << "Will enter foreground mono_ms=" << MonoMs();
  for (const auto& listener : Snapshot(g_will_enter_foreground)) {
    listener();
  }
}

void PlatformLifecycleEvents::OnDidEnterForeground() {
  AppLifecycle::logger().info << "Did enter foreground mono_ms=" << MonoMs();
  for (const auto& listener : Snapshot(g_did_enter_foreground)) {
    listener();
  }
}

int64_t PlatformLifecycleEvents::MonoMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

} // namespace pbr
