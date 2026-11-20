#include "foundation/platform/PlatformShareFile.h"

namespace pbr {

// No file share sheet here: a computer saves a received file with "Save as" instead.
bool PlatformCanShareFile() {
  return false;
}

bool PlatformShareFile(SDL_Window* /*window*/, const std::string& /*path*/, const std::string& /*mime*/,
                       ShareAnchor /*anchor*/) {
  return false;
}

} // namespace pbr
