#include "base/platform/SdlAppEvents.h"

#include "base/platform/AppLifecycle.h"
#include "base/platform/PlatformNavigation.h"
#include "base/ui/ContextMenuHost.h"
#include "base/ui/Theme.h"

#include "RmlUi_Backend.h"

#include <RmlUi/Core/Context.h>
#include <SDL3/SDL.h>

namespace pbr {

void SdlAppEvents::Install() {
  Backend::SetPreProcessEventHandler(&SdlAppEvents::PreProcess);
}

bool SdlAppEvents::PreProcess(Rml::Context* context, SDL_Event& event, bool& propagate_event) {
  (void)propagate_event;
  switch (event.type) {
  case SDL_EVENT_KEY_DOWN:
    if (event.key.key == SDLK_AC_BACK) {
      return PlatformNavigation::OnSystemBack();
    }
    if (event.key.key == SDLK_ESCAPE) {
      return PlatformNavigation::OnDismissKey();
    }
    break;
  case SDL_EVENT_WILL_ENTER_BACKGROUND:
    AppLifecycle::OnWillEnterBackground();
    return true;
  case SDL_EVENT_DID_ENTER_FOREGROUND:
    AppLifecycle::OnDidEnterForeground();
    if (context) {
      // Resume may restore EGL without a device-reset event; re-sync size/viewport.
      Backend::SyncContext(context);
      Theme::SyncSystemTheme(context);
    }
    return true;
  case SDL_EVENT_SYSTEM_THEME_CHANGED:
    if (context) {
      Theme::SyncSystemTheme(context);
    }
    return true;
  case SDL_EVENT_LOW_MEMORY:
    AppLifecycle::OnLowMemory();
    return true;
  case SDL_EVENT_MOUSE_BUTTON_DOWN:
    if (event.button.button == SDL_BUTTON_RIGHT && context) {
      const float scale = context->GetDensityIndependentPixelRatio();
      const int x = static_cast<int>(event.button.x * scale);
      const int y = static_cast<int>(event.button.y * scale);
      if (ContextMenuHost::Instance().OnContextPointer(context, x, y)) {
        propagate_event = false;
        return true;
      }
    }
    break;
  default:
    break;
  }
  return false;
}

} // namespace pbr
