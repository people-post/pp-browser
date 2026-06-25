#include "platform/AppLifecycle.h"

#include "demo/ChatDemo.h"
#include "common/Logger.h"
#include "platform/BrowserThread.h"

namespace pbr {

namespace {

AppLifecycleState g_state = AppLifecycleState::Foreground;

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
  ChatDemo::Instance().OnApplicationPause();
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

} // namespace pbr
