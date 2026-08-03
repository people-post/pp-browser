#include "base/platform/PlatformRuntime.h"

#include "base/platform/BrowserThread.h"
#include "base/platform/IAssetLocator.h"
#include "base/platform/ILocalNotifier.h"
#include "base/platform/IPathProvider.h"
#include "base/platform/PlatformServices.h"

namespace pbr {

namespace {

bool g_platform_services_registered = false;

} // namespace

void PlatformRuntime::EnsurePlatformServices() {
  if (g_platform_services_registered) {
    return;
  }
  PlatformServices::Register();
  g_platform_services_registered = true;
}

void PlatformRuntime::PostUI(std::function<void()> task) {
  if (!task) {
    return;
  }
  BrowserThread::PostTask(BrowserThreadId::UI, std::move(task));
}

IPathProvider& PlatformRuntime::Paths() {
  return IPathProvider::Instance();
}

IAssetLocator& PlatformRuntime::Assets() {
  return IAssetLocator::Instance();
}

ILocalNotifier& PlatformRuntime::Notifier() {
  return ILocalNotifier::Instance();
}

Rml::FileInterface* PlatformRuntime::PackagedFileInterface() {
  return PlatformServices::PackagedFileInterface();
}

} // namespace pbr
