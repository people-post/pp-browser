#pragma once

#include <SDL3/SDL.h>

namespace pbr {

/**
 * Linux: prefer ALSA when the user did not pin SDL_AUDIO_DRIVER.
 * pipewire-pulse + libpulse can abort the process in pa_make_fd_cloexec on open
 * (dogfood: RefreshPendingRing → CallRingtone → SDL_OpenAudioDeviceStream).
 * ALSA usually reaches the same PipeWire graph via the ALSA plugin.
 */
inline void PreferLinuxAlsaAudioDriverIfUnset() {
#if defined(__linux__)
  const char* driver = SDL_GetHint(SDL_HINT_AUDIO_DRIVER);
  if (!driver || !driver[0]) {
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "alsa");
  }
#endif
}

inline bool EnsureSdlAudioSubsystem() {
  PreferLinuxAlsaAudioDriverIfUnset();
  if (SDL_WasInit(SDL_INIT_AUDIO)) {
    return true;
  }
  return SDL_InitSubSystem(SDL_INIT_AUDIO);
}

} // namespace pbr
