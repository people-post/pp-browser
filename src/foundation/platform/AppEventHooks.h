#pragma once

#include "foundation/platform/ui/DropCollector.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ui {
class Context;
}

namespace pbr {

/** Optional UI hooks for SDL pre-processing (theme sync, context menu). Wired from app init. */
struct AppEventHooks {
  std::function<void(ui::Context*)> on_sync_system_theme;
  std::function<bool(ui::Context*, int x, int y)> on_context_pointer;
  /**
   * Files dragged over the window (a computer's native window only; DropCollector): the pointer's place in UI
   * coordinates, or empty = hide the hint (the drag ended, left, lost focus, or sat still for a second).
   */
  std::function<void(std::optional<DropUiPoint>)> on_drop_hover;
  /** The files that were dropped, and where the pointer last was (empty = this drag never had a position). */
  std::function<void(std::vector<std::string> paths, std::optional<DropUiPoint> position)> on_files_dropped;
};

void SetAppEventHooks(AppEventHooks hooks);
const AppEventHooks& GetAppEventHooks();

/**
 * Whether the key-down being handled is one the keyboard repeated (a key held down). SdlAppEvents::PreProcess sets it
 * from the SDL event just before the key reaches the key bindings, which have no event of their own to ask.
 */
void SetKeyDownIsRepeat(bool repeat);
bool KeyDownIsRepeat();

} // namespace pbr
