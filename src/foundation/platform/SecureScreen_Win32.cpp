#include "foundation/platform/SecureScreen.h"

#include <SDL3/SDL.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace pbr::SecureScreen {

/**
 * Every app window left out of screen capture (Windows 10 2004+: screen sharing, screenshots and recordings skip it);
 * older Windows shows it black instead (WDA_MONITOR).
 */
void SetSecretOnScreen(const bool on) {
  int count = 0;
  SDL_Window** windows = SDL_GetWindows(&count);
  if (windows == nullptr) {
    return;
  }
  for (int i = 0; i < count; ++i) {
    auto* hwnd = static_cast<HWND>(
        SDL_GetPointerProperty(SDL_GetWindowProperties(windows[i]), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (hwnd == nullptr) {
      continue;
    }
    if (!on) {
      (void)SetWindowDisplayAffinity(hwnd, WDA_NONE);
    } else if (!SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)) {
      (void)SetWindowDisplayAffinity(hwnd, WDA_MONITOR);
    }
  }
  SDL_free(windows);
}

void SetSecretLegible(const bool on) {
  (void)on;
}

} // namespace pbr::SecureScreen
