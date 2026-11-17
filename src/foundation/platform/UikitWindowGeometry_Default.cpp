#include "foundation/platform/UikitWindowGeometry.h"

namespace pbr {

UikitGeometryFix SyncUikitWindowGeometry(SDL_Window* /*window*/) {
  return {};
}

bool UikitViewFillsWindow(SDL_Window* /*window*/) {
  return true;
}

int WindowControlsTopInset(SDL_Window* /*window*/) {
  return 0;
}

bool IsIpadDevice() {
  return false;
}

std::string DescribeUikitWindowGeometry(SDL_Window* /*window*/) {
  return {};
}

} // namespace pbr
