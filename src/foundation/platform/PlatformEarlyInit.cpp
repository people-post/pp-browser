#include "foundation/platform/Platform.h"

#if defined(__ANDROID__)
#include "foundation/runtime/AppLifecycle.h"

#include <SDL3/SDL.h>
#elif defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include <SDL3/SDL.h>
#endif
#endif

namespace pbr {

#if defined(__ANDROID__)
namespace {

/**
 * B127: SDL hands the app's pause / resume only to event watches, while it pumps on the main
 * thread (SDL_SendAppEvent never queues them), so SdlAppEvents::PreProcess never sees them.
 */
bool SDLCALL AndroidLifecycleWatch(void* /*userdata*/, SDL_Event* event) {
  switch (event->type) {
  case SDL_EVENT_WILL_ENTER_BACKGROUND:
    AppLifecycle::OnWillEnterBackground();
    break;
  case SDL_EVENT_DID_ENTER_FOREGROUND:
    AppLifecycle::OnDidEnterForeground();
    break;
  default:
    break;
  }
  return true;
}

} // namespace
#endif

bool Platform::EarlyInit() {
#if defined(__ANDROID__)
  // The main loop keeps running when paused, so posted UI work (a background banner) still runs;
  // it stops drawing there instead (ShouldDrawFrame). SDL_Init(EVENTS) reads this hint.
  SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "0");
  if (!SDL_Init(SDL_INIT_EVENTS)) {
    return false;
  }
  SDL_SetHint("SDL_ANDROID_TRAP_BACK_BUTTON", "1");
  SDL_AddEventWatch(AndroidLifecycleWatch, nullptr);
#elif defined(__APPLE__) && TARGET_OS_IPHONE
  if (!SDL_Init(SDL_INIT_EVENTS)) {
    return false;
  }
#endif
  return true;
}

} // namespace pbr
