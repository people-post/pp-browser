#include "platform/PlatformNavigation.h"

#include "platform/Platform.h"
#include "ui/ShellHost.h"

#include "RmlUi_Backend.h"

#include <SDL3/SDL.h>

namespace pbr {

bool PlatformNavigation::OnDismissKey() {
  if (ShellHost::Instance().HandleDismiss()) {
    return true;
  }
  Backend::RequestExit();
  return true;
}

bool PlatformNavigation::OnSystemBack() {
  if (ShellHost::Instance().HandleDismiss()) {
    return true;
  }
  if (Platform::IsMobile()) {
    if (SDL_Window* window = Backend::GetWindow()) {
      SDL_MinimizeWindow(window);
    }
    return true;
  }
  Backend::RequestExit();
  return true;
}

} // namespace pbr
