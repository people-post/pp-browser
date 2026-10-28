#include "foundation/platform/ui/PlatformNavigation.h"

#include "foundation/platform/Platform.h"

#include "foundation/platform/ui/RmlUi_Backend.h"

#include <SDL3/SDL.h>

namespace pbr {

namespace {

std::function<bool()> g_dismiss_handler;
std::function<int()> g_escape_key_handler;

bool TryDismiss() {
  return g_dismiss_handler && g_dismiss_handler();
}

} // namespace

void PlatformNavigation::SetDismissHandler(std::function<bool()> handler) {
  g_dismiss_handler = std::move(handler);
}

void PlatformNavigation::SetEscapeKeyHandler(std::function<int()> handler) {
  g_escape_key_handler = std::move(handler);
}

bool PlatformNavigation::OnDismissKey() {
  const int composer = g_escape_key_handler ? g_escape_key_handler() : 0;
  if (composer == 1) {
    return true;
  }
  if (TryDismiss()) {
    return true;
  }
  if (composer == 2) {
    return true; // Escape in the composer never quits the app (it used to, with the draft in it)
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
