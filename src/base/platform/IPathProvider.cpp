#include "platform/IPathProvider.h"

#include "platform/DesktopPathProvider.h"

namespace pbr {

namespace {
IPathProvider* g_provider = nullptr;
DesktopPathProvider g_desktop_default;
} // namespace

IPathProvider& IPathProvider::Instance() {
  return g_provider ? *g_provider : g_desktop_default;
}

void IPathProvider::SetInstance(IPathProvider* provider) {
  g_provider = provider;
}

} // namespace pbr
