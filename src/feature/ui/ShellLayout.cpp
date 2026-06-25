#include "feature/ui/ShellLayout.h"

namespace pbr {

LayoutMode ShellLayout::FromWidth(float width_dp, float breakpoint) {
  return width_dp < breakpoint ? LayoutMode::Compact : LayoutMode::Expanded;
}

const char* ShellLayout::LayoutModeString(LayoutMode mode) {
  return mode == LayoutMode::Compact ? "compact" : "expanded";
}

const char* ShellLayout::NavTabString(NavTab tab) {
  if (tab == NavTab::Settings) {
    return "settings";
  }
  if (tab == NavTab::Contacts) {
    return "contacts";
  }
  return "sessions";
}

void ShellLayout::SyncLayoutModeString(ShellState& state) {
  state.layout_mode_str = LayoutModeString(state.layout_mode);
}

void ShellLayout::SyncNavTabString(ShellState& state) {
  state.nav_tab_str = NavTabString(state.nav_tab);
}

const char* ShellLayout::NavContentKey(NavTab tab) {
  if (tab == NavTab::Settings) {
    return "settings";
  }
  if (tab == NavTab::Contacts) {
    return "contacts";
  }
  return "sidebar";
}

PaneVisibility ShellLayout::WhichPanesVisible(const ShellState& state) {
  PaneVisibility vis{};
  vis.primary = true;

  if (state.layout_mode == LayoutMode::Expanded) {
    vis.secondary = true;
    vis.auxiliary = state.auxiliary_open;
    return vis;
  }

  vis.compact_nav_page = !state.compact_chat_open;
  vis.compact_chat_overlay = state.compact_chat_open;
  vis.auxiliary_sheet = state.auxiliary_open;
  return vis;
}

} // namespace pbr
