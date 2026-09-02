#include "base/platform/PlatformServices.h"

#include "base/platform/AndroidAssetLocator.h"
#include "base/platform/AndroidLocalNotifier.h"
#include "base/platform/AndroidPathProvider.h"
#include "base/platform/AndroidPushDeviceRegistrar.h"
#include "base/platform/DesktopLocalNotifier.h"
#include "base/platform/IAssetLocator.h"
#include "base/platform/ILocalNotifier.h"
#include "base/platform/IPathProvider.h"
#include "base/platform/IPushDeviceRegistrar.h"
#include "base/platform/IosAssetLocator.h"
#include "base/platform/IosPathProvider.h"
#include "base/platform/Platform.h"
#include "base/platform/SdlAssetFileInterface.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace pbr {

namespace {

#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
SdlAssetFileInterface* g_packaged_file_interface = nullptr;
#endif

// Construct Module-backed notifiers lazily inside Register(). A namespace-scope
// global runs during dynamic init and can call logging::getLogger before the
// root logger static is initialized (SIOF → smoke-run segfault on Linux).
DesktopLocalNotifier& DesktopNotifier() {
  static DesktopLocalNotifier notifier;
  return notifier;
}

#if defined(__ANDROID__)
AndroidLocalNotifier& AndroidNotifier() {
  static AndroidLocalNotifier notifier;
  return notifier;
}
AndroidPushDeviceRegistrar& AndroidPushRegistrar() {
  static AndroidPushDeviceRegistrar registrar;
  return registrar;
}
#endif

} // namespace

void PlatformServices::Register() {
  const PlatformKind kind = Platform::Detect();
  if (kind == PlatformKind::Android) {
    static AndroidPathProvider paths;
    static AndroidAssetLocator assets;
    IPathProvider::SetInstance(&paths);
    IAssetLocator::SetInstance(&assets);
#if defined(__ANDROID__)
    static SdlAssetFileInterface file_interface;
    g_packaged_file_interface = &file_interface;
    ILocalNotifier::SetInstance(&AndroidNotifier());
    IPushDeviceRegistrar::SetInstance(&AndroidPushRegistrar());
#endif
  } else if (kind == PlatformKind::IOS) {
    static IosPathProvider paths;
    static IosAssetLocator assets;
    IPathProvider::SetInstance(&paths);
    IAssetLocator::SetInstance(&assets);
#if defined(__APPLE__) && TARGET_OS_IPHONE
    static SdlAssetFileInterface file_interface;
    g_packaged_file_interface = &file_interface;
#endif
    ILocalNotifier::SetInstance(&DesktopNotifier());
  } else {
    ILocalNotifier::SetInstance(&DesktopNotifier());
  }
}

Rml::FileInterface* PlatformServices::PackagedFileInterface() {
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
  return g_packaged_file_interface;
#else
  return nullptr;
#endif
}

} // namespace pbr
