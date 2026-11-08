#include "foundation/platform/SecureScreen.h"

#import <AppKit/AppKit.h>
#include <SDL3/SDL.h>

namespace pbr::SecureScreen {

/** Every app window out of screen sharing and screenshots (NSWindowSharingNone), where the capturing app honours it. */
void SetSecretOnScreen(const bool on) {
  int count = 0;
  SDL_Window** windows = SDL_GetWindows(&count);
  if (windows == nullptr) {
    return;
  }
  for (int i = 0; i < count; ++i) {
    void* cocoa =
        SDL_GetPointerProperty(SDL_GetWindowProperties(windows[i]), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    if (cocoa == nullptr) {
      continue;
    }
    NSWindow* window = (__bridge NSWindow*)cocoa;
    window.sharingType = on ? NSWindowSharingNone : NSWindowSharingReadOnly;
  }
  SDL_free(windows);
}

void SetSecretLegible(const bool on) {
  (void)on;
}

} // namespace pbr::SecureScreen
