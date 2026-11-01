#include "foundation/platform/Platform.h"
#include "foundation/runtime/AppLifecycle.h"

#include <SDL3/SDL.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace pbr {

namespace {

/**
 * B127: SDL hands the app's pause / resume (iOS and Android) only to event watches, on the main
 * thread as it pumps (SDL_SendAppEvent never queues them), so SdlAppEvents::PreProcess never sees them.
 * Desktops send none of these; their minimized / restored window events take the pump path.
 */
bool SDLCALL AppLifecycleWatch(void* /*userdata*/, SDL_Event* event) {
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

bool Platform::EarlyInit() {
#if defined(__ANDROID__)
  // The main loop keeps running when paused, so posted UI work (a background banner) still runs;
  // it stops drawing there instead (ShouldDrawFrame). SDL_Init(EVENTS) reads this hint.
  SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "0");
  if (!SDL_Init(SDL_INIT_EVENTS)) {
    return false;
  }
  SDL_SetHint("SDL_ANDROID_TRAP_BACK_BUTTON", "1");
#elif defined(__APPLE__) && TARGET_OS_IPHONE
  if (!SDL_Init(SDL_INIT_EVENTS)) {
    return false;
  }
#endif
  SDL_AddEventWatch(AppLifecycleWatch, nullptr);  // before the first pause can arrive (B127)
  return true;
}

} // namespace pbr
