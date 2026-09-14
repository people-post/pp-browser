#pragma once

#include <functional>

namespace ui {
class Context;
}

namespace pbr {

/** Optional UI hooks for SDL pre-processing (theme sync, context menu). Wired from app init. */
struct AppEventHooks {
  std::function<void(ui::Context*)> on_sync_system_theme;
  std::function<bool(ui::Context*, int x, int y)> on_context_pointer;
};

void SetAppEventHooks(AppEventHooks hooks);
const AppEventHooks& GetAppEventHooks();

} // namespace pbr
