#include "base/platform/Platform.h"

#if defined(__ANDROID__)
#include <SDL3/SDL.h>
#elif defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include <SDL3/SDL.h>
#endif
#endif

namespace pbr {

bool Platform::EarlyInit() {
#if defined(__ANDROID__)
  if (!SDL_Init(SDL_INIT_EVENTS)) {
    return false;
  }
  SDL_SetHint("SDL_ANDROID_TRAP_BACK_BUTTON", "1");
#elif defined(__APPLE__) && TARGET_OS_IPHONE
  if (!SDL_Init(SDL_INIT_EVENTS)) {
    return false;
  }
#endif
  return true;
}

} // namespace pbr
