#include "platform/Platform.h"

#if defined(__ANDROID__)
#include <SDL3/SDL.h>
#endif

namespace pbr {

bool Platform::EarlyInit() {
#if defined(__ANDROID__)
  if (!SDL_Init(SDL_INIT_EVENTS)) {
    return false;
  }
  SDL_SetHint("SDL_ANDROID_TRAP_BACK_BUTTON", "1");
#endif
  return true;
}

} // namespace pbr
