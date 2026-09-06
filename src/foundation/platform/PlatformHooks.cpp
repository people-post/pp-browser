#include "foundation/platform/PlatformHooks.h"

#include "foundation/platform/AndroidAssetLocator.h"
#include "foundation/platform/AndroidLocalNotifier.h"
#include "foundation/platform/AndroidPathProvider.h"
#include "foundation/platform/AndroidPushDeviceRegistrar.h"
#include "foundation/platform/DesktopLocalNotifier.h"
#include "foundation/platform/IAssetLocator.h"
#include "foundation/platform/ILocalNotifier.h"
#include "foundation/platform/IPathProvider.h"
#include "foundation/platform/IPushDeviceRegistrar.h"
#include "foundation/platform/IosAssetLocator.h"
#include "foundation/platform/IosPathProvider.h"
#include "foundation/platform/Platform.h"
#include "foundation/platform/SdlAssetFileInterface.h"

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

void PlatformHooks::Register() {
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

Rml::FileInterface* PlatformHooks::PackagedFileInterface() {
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
  return g_packaged_file_interface;
#else
  return nullptr;
#endif
}

} // namespace pbr
