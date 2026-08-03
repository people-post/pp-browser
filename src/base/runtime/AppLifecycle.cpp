#include "base/runtime/AppLifecycle.h"

#include "base/runtime/AppRuntime.h"
#include "common/Logger.h"

#include <atomic>
#include <vector>

namespace pbr {

namespace {

AppLifecycleState g_state = AppLifecycleState::Foreground;
std::vector<std::function<void()>> g_background_listeners;
std::vector<std::function<void()>> g_foreground_listeners;

// Updated only on the UI/SDL thread. Read from any thread for alert gating.
// Mobile never touches these; defaults keep IsUserAttentive() == IsForeground().
std::atomic<bool> g_desktop_input_focused{true};
std::atomic<bool> g_desktop_minimized{false};

} // namespace

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
    logging::getLogger("AppLifecycle").info
        << "Desktop input focus=" << (focused ? "gained" : "lost");
  }
}

void AppLifecycle::SetDesktopMinimized(bool minimized) {
  g_desktop_minimized.store(minimized, std::memory_order_relaxed);
}

void AppLifecycle::OnWillEnterBackground() {
  if (g_state == AppLifecycleState::Background) {
    return;
  }
  g_state = AppLifecycleState::Background;
  AppRuntime::PauseBackgroundWork();
  for (const auto& listener : g_background_listeners) {
    listener();
  }
  logging::getLogger("AppLifecycle").info << "Entering background";
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
  for (const auto& listener : g_foreground_listeners) {
    listener();
  }
  logging::getLogger("AppLifecycle").info << "Entering foreground";
}

void AppLifecycle::OnLowMemory() {
  logging::getLogger("AppLifecycle").warning << "Low memory warning";
}

void AppLifecycle::AddBackgroundListener(std::function<void()> listener) {
  if (listener) {
    g_background_listeners.push_back(std::move(listener));
  }
}

void AppLifecycle::AddForegroundListener(std::function<void()> listener) {
  if (listener) {
    g_foreground_listeners.push_back(std::move(listener));
  }
}

void AppLifecycle::ClearBackgroundListeners() {
  g_background_listeners.clear();
}

void AppLifecycle::ClearForegroundListeners() {
  g_foreground_listeners.clear();
}

} // namespace pbr
