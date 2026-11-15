#include "foundation/platform/UikitWindowGeometry.h"

namespace pbr {

UikitGeometryFix SyncUikitWindowGeometry(SDL_Window* /*window*/) {
  return {};
}

int WindowControlsTopInset(SDL_Window* /*window*/) {
  return 0;
}

std::string DescribeUikitWindowGeometry(SDL_Window* /*window*/) {
  return {};
}

} // namespace pbr
