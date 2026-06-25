#include "base/platform/AppLifecycle.h"

#include "common/Logger.h"
#include "base/platform/BrowserThread.h"

#include <vector>

namespace pbr {

namespace {

AppLifecycleState g_state = AppLifecycleState::Foreground;
std::vector<std::function<void()>> g_background_listeners;

} // namespace

AppLifecycleState AppLifecycle::Current() {
  return g_state;
}

bool AppLifecycle::IsForeground() {
  return g_state == AppLifecycleState::Foreground;
}

void AppLifecycle::OnWillEnterBackground() {
  if (g_state == AppLifecycleState::Background) {
    return;
  }
  g_state = AppLifecycleState::Background;
  BrowserThread::PauseIO();
  for (const auto& listener : g_background_listeners) {
    listener();
  }
  logging::getLogger("AppLifecycle").info << "Entering background";
}

void AppLifecycle::OnDidEnterForeground() {
  if (g_state == AppLifecycleState::Foreground) {
    return;
  }
  g_state = AppLifecycleState::Foreground;
  BrowserThread::ResumeIO();
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

void AppLifecycle::ClearBackgroundListeners() {
  g_background_listeners.clear();
}

} // namespace pbr
