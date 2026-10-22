#include "foundation/runtime/AppLifecycle.h"

#include "foundation/runtime/AppRuntime.h"
#include "common/Logger.h"

#include <atomic>
#include <mutex>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

AppLifecycleState g_state = AppLifecycleState::Foreground;
struct Listener {
  AppLifecycle::ListenerId id = 0;
  std::function<void()> fn;
};
// Added / removed from any thread (owners register as they come up); called on the UI thread.
std::mutex g_listeners_mu;
std::vector<Listener> g_background_listeners;
std::vector<Listener> g_foreground_listeners;
struct StayConnectedListener {
  AppLifecycle::ListenerId id = 0;
  std::function<void(bool)> fn;
};
std::vector<StayConnectedListener> g_stay_connected_listeners;
std::atomic<bool> g_stay_connected{false};
AppLifecycle::ListenerId g_next_listener_id = 1;

/** A copy taken under the lock: listeners run outside it (one may add or remove another). */
std::vector<std::function<void()>> Snapshot(const std::vector<Listener>& listeners) {
  std::lock_guard lock(g_listeners_mu);
  std::vector<std::function<void()>> out;
  out.reserve(listeners.size());
  for (const Listener& listener : listeners) {
    out.push_back(listener.fn);
  }
  return out;
}

AppLifecycle::ListenerId Add(std::vector<Listener>& listeners, std::function<void()> fn) {
  if (!fn) {
    return 0;
  }
  std::lock_guard lock(g_listeners_mu);
  const AppLifecycle::ListenerId id = g_next_listener_id++;
  listeners.push_back({id, std::move(fn)});
  return id;
}

// Updated only on the UI/SDL thread. Read from any thread for alert gating.
// Mobile never touches these; defaults keep IsUserAttentive() == IsForeground().
std::atomic<bool> g_desktop_input_focused{true};
std::atomic<bool> g_desktop_minimized{false};

std::mutex g_log_mu;
logging::Logger* g_log = nullptr;

} // namespace

void AppLifecycle::InitLogging() {
  std::lock_guard lock(g_log_mu);
  if (g_log != nullptr) {
    return;
  }
  (void)logging::getLogger("Runtime");
  static logging::Logger instance = logging::getLogger("Runtime.Lifecycle");
  g_log = &instance;
}

logging::Logger& AppLifecycle::logger() {
  InitLogging();
  return *g_log;
}

AppLifecycleState AppLifecycle::Current() {
  return g_state;
}

bool AppLifecycle::IsForeground() {
  return g_state == AppLifecycleState::Foreground;
}

bool AppLifecycle::IsUserAttentive() {
  if (!IsForeground()) {
    return false;
  }
  if (g_desktop_minimized.load(std::memory_order_relaxed)) {
    return false;
  }
  return g_desktop_input_focused.load(std::memory_order_relaxed);
}

void AppLifecycle::SetDesktopInputFocused(bool focused) {
  const bool prev = g_desktop_input_focused.exchange(focused, std::memory_order_relaxed);
  if (prev != focused) {
    logger().info
        << "Desktop input focus=" << (focused ? "gained" : "lost");
  }
}

void AppLifecycle::SetDesktopMinimized(bool minimized) {
  g_desktop_minimized.store(minimized, std::memory_order_relaxed);
}

void AppLifecycle::OnDesktopMinimized() {
  SetDesktopMinimized(true);
  SetDesktopInputFocused(false);
  logger().info << "Desktop window minimized (still working)";
}

void AppLifecycle::OnDesktopRestored() {
  SetDesktopMinimized(false);
}

void AppLifecycle::SetStayConnected(const bool on) {
  if (g_stay_connected.exchange(on) == on) {
    return;
  }
  logger().info << "Stay connected in the background: " << (on ? "on" : "off");
  if (g_state == AppLifecycleState::Background) {
    // Already in the background: follow the setting now.
    if (on) {
      AppRuntime::ResumeBackgroundWork();
    } else {
      AppRuntime::PauseBackgroundWork();
    }
  }
  std::vector<std::function<void(bool)>> listeners;
  {
    std::lock_guard lock(g_listeners_mu);
    for (const StayConnectedListener& listener : g_stay_connected_listeners) {
      listeners.push_back(listener.fn);
    }
  }
  for (const auto& listener : listeners) {
    listener(on);
  }
}

bool AppLifecycle::StaysConnected() {
  return g_stay_connected.load();
}

AppLifecycle::ListenerId AppLifecycle::AddStayConnectedListener(std::function<void(bool)> listener) {
  if (!listener) {
    return 0;
  }
  std::lock_guard lock(g_listeners_mu);
  const ListenerId id = g_next_listener_id++;
  g_stay_connected_listeners.push_back({id, std::move(listener)});
  return id;
}

void AppLifecycle::OnWillEnterBackground() {
  if (g_state == AppLifecycleState::Background) {
    return;
  }
  g_state = AppLifecycleState::Background;
  if (!g_stay_connected.load()) {
    AppRuntime::PauseBackgroundWork();  // push only (P010)
  }
  for (const auto& listener : Snapshot(g_background_listeners)) {
    listener();
  }
  logger().info << "Entering background";
}

void AppLifecycle::OnDidEnterForeground() {
  // Always ResumeBackgroundWork: Android can deliver WILL_ENTER_BACKGROUND without a matching
  // DID_ENTER_FOREGROUND after a surface blip, or the reverse order at cold start.
  // Resume is idempotent; skipping it when g_state was already Foreground left workers paused.
  const bool was_background = g_state == AppLifecycleState::Background;
  g_state = AppLifecycleState::Foreground;
  AppRuntime::ResumeBackgroundWork();
  if (!was_background) {
    return;
  }
  for (const auto& listener : Snapshot(g_foreground_listeners)) {
    listener();
  }
  logger().info << "Entering foreground";
}

void AppLifecycle::OnLowMemory() {
  logger().warning << "Low memory warning";
}

AppLifecycle::ListenerId AppLifecycle::AddBackgroundListener(std::function<void()> listener) {
  return Add(g_background_listeners, std::move(listener));
}

AppLifecycle::ListenerId AppLifecycle::AddForegroundListener(std::function<void()> listener) {
  return Add(g_foreground_listeners, std::move(listener));
}

void AppLifecycle::RemoveListener(const ListenerId id) {
  if (id == 0) {
    return;
  }
  std::lock_guard lock(g_listeners_mu);
  for (auto* listeners : {&g_background_listeners, &g_foreground_listeners}) {
    std::erase_if(*listeners, [id](const Listener& listener) { return listener.id == id; });
  }
  std::erase_if(g_stay_connected_listeners, [id](const StayConnectedListener& listener) { return listener.id == id; });
}

} // namespace pbr
