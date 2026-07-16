#include "base/platform/SdlAppEvents.h"

#include "base/platform/AppEventHooks.h"
#include "base/platform/AppLifecycle.h"
#include "base/platform/PlatformNavigation.h"

#include "RmlUi_Backend.h"

#include <RmlUi/Core/Context.h>
#include <SDL3/SDL.h>

namespace pbr {

void SdlAppEvents::Install() {
  Backend::SetPreProcessEventHandler(&SdlAppEvents::PreProcess);
}

bool SdlAppEvents::PreProcess(Rml::Context* context, SDL_Event& event, bool& propagate_event) {
  (void)propagate_event;
  const AppEventHooks& hooks = GetAppEventHooks();
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
    if (context && hooks.on_sync_system_theme) {
      // Resume may restore EGL without a device-reset event; re-sync size/viewport.
      Backend::SyncContext(context);
      hooks.on_sync_system_theme(context);
    }
    return true;
  case SDL_EVENT_SYSTEM_THEME_CHANGED:
    if (context && hooks.on_sync_system_theme) {
      hooks.on_sync_system_theme(context);
    }
    return true;
  case SDL_EVENT_LOW_MEMORY:
    AppLifecycle::OnLowMemory();
    return true;
  case SDL_EVENT_MOUSE_BUTTON_DOWN:
    if (event.button.button == SDL_BUTTON_RIGHT && context && hooks.on_context_pointer) {
      const float scale = context->GetDensityIndependentPixelRatio();
      const int x = static_cast<int>(event.button.x * scale);
      const int y = static_cast<int>(event.button.y * scale);
      if (hooks.on_context_pointer(context, x, y)) {
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
