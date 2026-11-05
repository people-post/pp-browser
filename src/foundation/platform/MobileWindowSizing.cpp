#include "foundation/platform/MobileWindowSizing.h"

#include "foundation/platform/Platform.h"

#include <SDL3/SDL.h>

namespace pbr {

void ResolveMobileWindowSize(int& width, int& height) {
  if (!Platform::IsMobile()) {
    return;
  }
  if (!SDL_WasInit(SDL_INIT_VIDEO)) {
    SDL_InitSubSystem(SDL_INIT_VIDEO);
  }
  const SDL_DisplayID display = SDL_GetPrimaryDisplay();
  const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(display);
  if (mode && mode->w > 0 && mode->h > 0) {
    width = mode->w;
    height = mode->h;
  }
}

float PrimaryDisplayRefreshHz() {
  if (!SDL_WasInit(SDL_INIT_VIDEO)) {
    return 0.0f;
  }
  const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
  return mode ? mode->refresh_rate : 0.0f;
}

} // namespace pbr
