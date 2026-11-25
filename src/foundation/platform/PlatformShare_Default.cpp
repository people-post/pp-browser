#include "foundation/platform/PlatformShare.h"

namespace pbr {

bool PlatformShareText(SDL_Window* /*window*/, const std::string& /*text*/, ShareAnchor /*anchor*/) {
  return false;
}

bool PlatformCanShareText() {
  return false;
}

bool PlatformSystemModalUp(SDL_Window* /*window*/) {
  return false;
}

} // namespace pbr
