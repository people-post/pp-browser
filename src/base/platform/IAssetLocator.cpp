#include "base/platform/IAssetLocator.h"

#include "base/platform/DesktopAssetLocator.h"

namespace pbr {

namespace {
IAssetLocator* g_locator = nullptr;
DesktopAssetLocator g_desktop_default;
} // namespace

IAssetLocator& IAssetLocator::Instance() {
  return g_locator ? *g_locator : g_desktop_default;
}

void IAssetLocator::SetInstance(IAssetLocator* locator) {
  g_locator = locator;
}

} // namespace pbr
