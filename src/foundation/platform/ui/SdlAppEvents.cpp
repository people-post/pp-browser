#include "foundation/platform/ui/SdlAppEvents.h"

#include "common/Logger.h"
#include "foundation/platform/AppEventHooks.h"
#include "foundation/platform/ui/DropCollector.h"
#include "foundation/platform/IncomingLink.h"
#include "foundation/runtime/AppLifecycle.h"
#include "foundation/platform/Platform.h"
#include "foundation/platform/ui/PlatformNavigation.h"

#include "foundation/platform/ui/RmlUi_Backend.h"

#include <ui/dom/Context.h>
#include <SDL3/SDL.h>

#include <string>
#include <utility>

namespace pbr {
namespace {

void MaybeEnterDesktopForeground() {
  if (!Platform::IsDesktop()) {
    return;
  }
  SDL_Window* window = Backend::GetWindow();
  if (!window) {
    return;
  }
  const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
  if ((flags & SDL_WINDOW_MINIMIZED) != 0) {
    return;
  }
  AppLifecycle::OnDidEnterForeground();
}

SdlAppEvents::FingerHandler& FingerHandlerSlot() {
  static SdlAppEvents::FingerHandler handler;
  return handler;
}

DropCollector& Collector() {
  static DropCollector collector;
  return collector;
}

bool& FirstPositionSeen() {
  static bool seen = false;
  return seen;
}

/** Files dragged onto the window are the drop collector's only on a computer with the hooks wired. */
bool DropCollected(const AppEventHooks& hooks) {
  return Platform::IsDesktop() && hooks.on_files_dropped;
}

int64_t NowMs() {
  return static_cast<int64_t>(SDL_GetTicks());
}

/** The engine's mouse path multiplies window coordinates by this (RmlSDL::InputEventHandler). */
float WindowPixelDensity() {
  SDL_Window* window = Backend::GetWindow();
  const float density = window ? SDL_GetWindowPixelDensity(window) : 1.f;
  return density > 0.f ? density : 1.f;
}

std::optional<DropUiPoint> ToUi(const std::optional<DropPoint>& point) {
  if (!point) {
    return std::nullopt;
  }
  return DropPointToUi(*point, WindowPixelDensity());
}

void HideDropHint(const AppEventHooks& hooks) {
  if (hooks.on_drop_hover) {
    hooks.on_drop_hover(std::nullopt);
  }
}

} // namespace

void SdlAppEvents::TickDrop() {
  if (Collector().Tick(NowMs())) {
    HideDropHint(GetAppEventHooks());
  }
}

std::optional<int64_t> SdlAppEvents::MsUntilDropHintHide() {
  return Collector().MsUntilHintHide(NowMs());
}

void SdlAppEvents::ResetDropForTest() {
  Collector() = DropCollector();
  FirstPositionSeen() = false;
}

void SdlAppEvents::SetFingerHandler(FingerHandler handler) {
  FingerHandlerSlot() = std::move(handler);
}

void SdlAppEvents::Install() {
  Backend::SetPreProcessEventHandler(&SdlAppEvents::PreProcess);
}

bool SdlAppEvents::PreProcess(ui::Context* context, SDL_Event& event, bool& propagate_event) {
  (void)propagate_event;
  if (FingerHandlerSlot()) {
    const bool is_finger = event.type == SDL_EVENT_FINGER_DOWN || event.type == SDL_EVENT_FINGER_MOTION ||
                           event.type == SDL_EVENT_FINGER_UP || event.type == SDL_EVENT_FINGER_CANCELED;
    if (is_finger || event.type == SDL_EVENT_WINDOW_FOCUS_LOST || event.type == SDL_EVENT_WINDOW_MINIMIZED ||
        event.type == SDL_EVENT_WILL_ENTER_BACKGROUND || event.type == SDL_EVENT_DID_ENTER_BACKGROUND) {
      const FingerHandler handler = FingerHandlerSlot(); // a copy: the handler may clear the slot (the page closing)
      if (handler(context, event) && is_finger) {
        return true;
      }
    }
  }
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
        Backend::SyncContext(context);
        hooks.on_sync_system_theme(context);
      }
      return true;
    case SDL_EVENT_WINDOW_MINIMIZED:
      if (Platform::IsDesktop()) {
        // Not background: a minimized messenger still rings and receives (it pausing all work held
        // every incoming call until the window came back).
        AppLifecycle::OnDesktopMinimized();
      }
      break;
    case SDL_EVENT_WINDOW_RESTORED:
      if (Platform::IsDesktop()) {
        AppLifecycle::OnDesktopRestored();
      }
      MaybeEnterDesktopForeground();
      break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      if (Platform::IsDesktop()) {
        AppLifecycle::SetDesktopInputFocused(false);
      }
      if (Collector().OnFocusLost()) {
        HideDropHint(hooks);
      }
      break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
      if (Platform::IsDesktop()) {
        AppLifecycle::SetDesktopInputFocused(true);
      }
      MaybeEnterDesktopForeground();
      break;
    case SDL_EVENT_SYSTEM_THEME_CHANGED:
      if (context && hooks.on_sync_system_theme) {
        hooks.on_sync_system_theme(context);
      }
      return true;
    case SDL_EVENT_LOW_MEMORY:
      AppLifecycle::OnLowMemory();
      return true;
    case SDL_EVENT_DROP_BEGIN:
      // Not relied on (SDL sends it once per window until a COMPLETE; a drop macOS rejects sends none): when it
      // does come, it is a new drag.
      if (Collector().HintVisible()) {
        Collector().OnBegin();
        HideDropHint(hooks);
      } else {
        Collector().OnBegin();
      }
      break;
    case SDL_EVENT_DROP_POSITION:
      if (DropCollected(hooks)) {
        if (!FirstPositionSeen()) {
          FirstPositionSeen() = true;
          logging::getLogger("Platform.Drop").debug << "drop: hover positions available";
        }
        Collector().OnPosition(event.drop.x, event.drop.y, NowMs());
        if (hooks.on_drop_hover) {
          hooks.on_drop_hover(ToUi(Collector().LastPosition()));
        }
        return true;
      }
      break;
    case SDL_EVENT_DROP_FILE:
      // macOS and iOS hand over an opened URL (a registered scheme) as a drop: a link, never a dropped file.
      if (event.drop.data && IncomingLink::LooksLikeLink(event.drop.data)) {
        IncomingLink::Deliver(event.drop.data);
        return true;
      }
      if (DropCollected(hooks)) {
        if (event.drop.data) {
          Collector().OnFile(std::string(event.drop.data)); // SDL's string dies at the next poll: copied now
        }
        return true;
      }
      break;
    case SDL_EVENT_DROP_TEXT:
      if (DropCollected(hooks)) {
        Collector().OnText();
        return true;
      }
      break;
    case SDL_EVENT_DROP_COMPLETE:
      if (DropCollected(hooks)) {
        DropCollector::Completed done = Collector().OnComplete();
        HideDropHint(hooks);
        if (!done.left) {
          hooks.on_files_dropped(std::move(done.paths), ToUi(done.position));
        }
        return true;
      }
      break;
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
