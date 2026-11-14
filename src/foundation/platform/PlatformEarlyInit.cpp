#include "foundation/platform/Platform.h"
#include "foundation/runtime/AppLifecycle.h"

#include <SDL3/SDL.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(__ANDROID__)
#include <dlfcn.h>
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
    case SDL_EVENT_DID_ENTER_BACKGROUND:
      AppLifecycle::OnDidEnterBackground();
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
  // B215: SDL reads this only in SDL_VideoInit and, finding it unset, turns the OS's screen sleep off for good. On
  // phones ResolveMobileWindowSize starts video before BrowserHost does, so the hint is set here: main() calls this
  // before anything else touches SDL (the arg loop and logging above it use no SDL), on every platform.
  SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");
#if defined(__ANDROID__)
  // B126: something closes a file descriptor it does not own (another unique_fd's) during group video; fdsan's
  // default for apps aborts the process. Until it is found, warn once instead: Android still logs the offending
  // close() with its backtrace (logcat, a non-fatal debuggerd dump), and the call goes on. API 29+ only, so looked
  // up at run time (minSdk 24). 1 = ANDROID_FDSAN_ERROR_LEVEL_WARN_ONCE (<android/fdsan.h>).
  using FdsanSetErrorLevel = int (*)(int);
  if (auto set_level = reinterpret_cast<FdsanSetErrorLevel>(dlsym(RTLD_DEFAULT, "android_fdsan_set_error_level"))) {
    set_level(1);
  }
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
  SDL_AddEventWatch(AppLifecycleWatch, nullptr); // before the first pause can arrive (B127)
  return true;
}

} // namespace pbr
