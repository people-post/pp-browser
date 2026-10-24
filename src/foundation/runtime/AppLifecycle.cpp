#include "foundation/runtime/AppLifecycle.h"

#include "foundation/runtime/AppRuntime.h"
#include "common/Logger.h"

#include <atomic>
#include <mutex>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

// Written on the UI thread; read from any (alert gating, the work reconcile).
std::atomic<AppLifecycleState> g_state{AppLifecycleState::Foreground};
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
std::atomic<bool> g_call_in_progress{false};
AppLifecycle::ListenerId g_next_listener_id = 1;

/**
 * Whether the app's work is paused, and what keeps it running in the background: stay connected
 * (P011), a call, a wake window (P010). Pause / Resume only set flags, so any thread may reconcile.
 */
std::mutex g_work_mu;
bool g_work_paused = false;
/** The open wake window's generation (0: none); a newer window replaces it. */
uint64_t g_wake_window = 0;
uint64_t g_next_wake_window = 1;

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

namespace {

const char* KeepsWorkingReason() {
  if (g_stay_connected.load()) {
    return "stay connected";
  }
  if (g_call_in_progress.load()) {
    return "call";
  }
  if (g_wake_window != 0) {
    return "wake window";
  }
  return nullptr;
}

/** Pause the work in the background unless something keeps it running; resume otherwise. */
void ReconcileWorkLocked() {
  const bool pause = g_state == AppLifecycleState::Background && KeepsWorkingReason() == nullptr;
  if (pause == g_work_paused) {
    return;
  }
  g_work_paused = pause;
  if (pause) {
    AppRuntime::PauseBackgroundWork();  // push only (P010)
    AppLifecycle::logger().info << "Background: work paused (push only)";
  } else {
    AppRuntime::ResumeBackgroundWork();
    AppLifecycle::logger().info << "Background: work runs (" << (KeepsWorkingReason() ? KeepsWorkingReason() : "foreground")
                                << ")";
  }
}

void ReconcileWork() {
  std::lock_guard lock(g_work_mu);
  ReconcileWorkLocked();
}

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
  ReconcileWork();  // already in the background: follow the setting now
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

void AppLifecycle::SetCallInProgress(const bool on) {
  if (g_call_in_progress.exchange(on) == on) {
    return;
  }
  if (on) {
    ReconcileWork();
    return;
  }
  // Not paused at once: the call's last signaling (the leave, an ack) is still going out.
  OpenWakeWindow(kCallEndGraceMs);
}

bool AppLifecycle::CallInProgress() {
  return g_call_in_progress.load();
}

void AppLifecycle::OpenWakeWindow(const std::chrono::milliseconds window) {
  uint64_t generation = 0;
  {
    std::lock_guard lock(g_work_mu);
    generation = g_next_wake_window++;
    g_wake_window = generation;
    ReconcileWorkLocked();
  }
  AppRuntime::ScheduleCoordinatorOneShot(window, [generation]() {
    std::lock_guard lock(g_work_mu);
    if (g_wake_window != generation) {
      return;  // a newer window replaced it
    }
    g_wake_window = 0;
    ReconcileWorkLocked();
  });
}

void AppLifecycle::OnWillEnterBackground() {
  if (g_state == AppLifecycleState::Background) {
    return;
  }
  g_state = AppLifecycleState::Background;
  ReconcileWork();
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
  {
    std::lock_guard lock(g_work_mu);
    g_work_paused = false;
    AppRuntime::ResumeBackgroundWork();
  }
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
