#include "domain/media/SdlAudioBootstrap.h"

#include <SDL3/SDL.h>

namespace pbr {

void PreferLinuxAlsaAudioDriverIfUnset() {
  const char* driver = SDL_GetHint(SDL_HINT_AUDIO_DRIVER);
  if (!driver || !driver[0]) {
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "alsa");
  }
}

} // namespace pbr
