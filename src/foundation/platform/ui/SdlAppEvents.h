#pragma once

namespace ui {
class Context;
}

#include <SDL3/SDL.h>

#include <functional>

namespace pbr {

class SdlAppEvents {
public:
  static void Install();
  static bool PreProcess(ui::Context* context, SDL_Event& event, bool& propagate_event);

  /**
   * A page that takes finger events itself (the avatar crop page: the UI library would turn every finger into a mouse
   * press). While one is set, PreProcess hands it every SDL_EVENT_FINGER_* and the events that mean "forget the
   * fingers" (window focus lost, minimised, the app going to the background) before anything else sees them; for a
   * finger event, true means taken and the UI library never sees it. Set when the page opens, cleared when it closes
   * (an empty function clears). UI thread, the event loop's thread.
   */
  using FingerHandler = std::function<bool(ui::Context* context, const SDL_Event& event)>;
  static void SetFingerHandler(FingerHandler handler);
};

} // namespace pbr
