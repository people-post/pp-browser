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

DesktopLocalNotifier g_desktop_notifier;
#if defined(__ANDROID__)
AndroidLocalNotifier g_android_notifier;
AndroidPushDeviceRegistrar g_android_push_registrar;
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
    ILocalNotifier::SetInstance(&g_android_notifier);
    IPushDeviceRegistrar::SetInstance(&g_android_push_registrar);
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
    ILocalNotifier::SetInstance(&g_desktop_notifier);
  } else {
    ILocalNotifier::SetInstance(&g_desktop_notifier);
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
