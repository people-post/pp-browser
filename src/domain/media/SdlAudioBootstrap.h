#pragma once

#include <SDL3/SDL.h>

namespace pbr {

/**
 * Prefer a stable SDL audio driver when unset (Linux: ALSA).
 * pipewire-pulse + libpulse can abort in pa_make_fd_cloexec on open
 * (dogfood: RefreshPendingRing → CallRingtone → SDL_OpenAudioDeviceStream).
 * ALSA usually reaches the same PipeWire graph via the ALSA plugin.
 * No-op on non-Linux backends (CMake source-selects SdlAudioBootstrap_*.cpp).
 */
void PreferLinuxAlsaAudioDriverIfUnset();

inline bool EnsureSdlAudioSubsystem() {
  PreferLinuxAlsaAudioDriverIfUnset();
  if (SDL_WasInit(SDL_INIT_AUDIO)) {
    return true;
  }
  return SDL_InitSubSystem(SDL_INIT_AUDIO);
}

} // namespace pbr
