#include "foundation/platform/PlatformShare.h"

namespace pbr {

bool PlatformShareText(SDL_Window* /*window*/, const std::string& /*text*/, ShareAnchor /*anchor*/) {
  return false;
}

} // namespace pbr
