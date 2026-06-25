#include "platform/PlatformServices.h"

#include "platform/AndroidAssetLocator.h"
#include "platform/AndroidPathProvider.h"
#include "platform/IAssetLocator.h"
#include "platform/IPathProvider.h"
#include "platform/IosAssetLocator.h"
#include "platform/IosPathProvider.h"
#include "platform/Platform.h"
#include "platform/SdlAssetFileInterface.h"

namespace pbr {

namespace {

#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
SdlAssetFileInterface* g_packaged_file_interface = nullptr;
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
