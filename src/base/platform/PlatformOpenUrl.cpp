#include "base/platform/PlatformOpenUrl.h"

#include "common/Logger.h"

#include <SDL3/SDL.h>

namespace pbr {

bool PlatformOpenUrl(const std::string& url) {
  if (url.empty()) {
    return false;
  }
  if (!SDL_OpenURL(url.c_str())) {
    logging::getLogger("PlatformOpenUrl").warning
        << "SDL_OpenURL failed for " << url << ": " << SDL_GetError();
    return false;
  }
  return true;
}

} // namespace pbr
