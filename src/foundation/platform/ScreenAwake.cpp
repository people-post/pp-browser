#include "foundation/platform/ScreenAwake.h"

#include "common/Logger.h"
#include "common/PbrCompat.h"

#include <SDL3/SDL.h>

namespace pbr::ScreenAwake {

void SetKeepScreenOn(const bool on, const char* reason) {
  const bool ok = on ? SDL_DisableScreenSaver() : SDL_EnableScreenSaver();
  if (!ok) {
    logging::getLogger("ScreenAwake").warning << "screen awake " << (on ? "on" : "off") << " failed reason=" << reason
                                              << " error=" << SDL_GetError();
    return;
  }
  logging::getLogger("ScreenAwake").info << "screen awake " << (on ? "on" : "off") << " reason=" << reason
                                         << " allowed_by_os=" << (SDL_ScreenSaverEnabled() ? 1 : 0);
}

void LogOsState(const char* reason) {
  logging::getLogger("ScreenAwake").info << "screen awake " << reason
                                         << " allowed_by_os=" << (SDL_ScreenSaverEnabled() ? 1 : 0);
}

} // namespace pbr::ScreenAwake
