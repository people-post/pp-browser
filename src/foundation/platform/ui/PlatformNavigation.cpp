#include "foundation/platform/ui/PlatformNavigation.h"

#include "foundation/platform/Platform.h"

#include "foundation/platform/ui/RmlUi_Backend.h"

#include <SDL3/SDL.h>

namespace pbr {

namespace {

std::function<bool()> g_dismiss_handler;

bool TryDismiss() {
  return g_dismiss_handler && g_dismiss_handler();
}

} // namespace

void PlatformNavigation::SetDismissHandler(std::function<bool()> handler) {
  g_dismiss_handler = std::move(handler);
}

bool PlatformNavigation::OnDismissKey() {
  if (TryDismiss()) {
    return true;
  }
  Backend::RequestExit();
  return true;
}

bool PlatformNavigation::OnSystemBack() {
  if (TryDismiss()) {
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
