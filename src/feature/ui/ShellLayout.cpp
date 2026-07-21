#include "feature/ui/ShellLayout.h"

#include <algorithm>

namespace pbr {

LayoutMode ShellLayout::FromWidth(float width_dp, float breakpoint) {
  return width_dp < breakpoint ? LayoutMode::Compact : LayoutMode::Expanded;
}

const char* ShellLayout::LayoutModeString(LayoutMode mode) {
  return mode == LayoutMode::Compact ? "compact" : "expanded";
}

const char* ShellLayout::NavTabString(NavTab tab) {
  if (tab == NavTab::Me) {
    return "me";
  }
  if (tab == NavTab::Contacts) {
    return "contacts";
  }
  if (tab == NavTab::Sessions) {
    return "sessions";
  }
  return "home";
}

void ShellLayout::SyncLayoutModeString(ShellState& state) {
  state.layout_mode_str = LayoutModeString(state.layout_mode);
}

void ShellLayout::SyncNavTabString(ShellState& state) {
  state.nav_tab_str = NavTabString(state.nav_tab);
}

const char* ShellLayout::NavContentKey(NavTab tab) {
  if (tab == NavTab::Home) {
    return nullptr;
  }
  if (tab == NavTab::Me) {
    return "settings";
  }
  if (tab == NavTab::Contacts) {
    return "contacts";
  }
  return "sidebar";
}

bool ShellLayout::TabHasSecondary(NavTab tab) {
  return NavContentKey(tab) != nullptr;
}

PaneVisibility ShellLayout::WhichPanesVisible(const ShellState& state) {
  PaneVisibility vis{};
  vis.primary = true;

  if (state.layout_mode == LayoutMode::Expanded) {
    vis.secondary = true;
    vis.auxiliary = state.auxiliary_open;
    return vis;
  }

  vis.compact_nav_page = !state.compact_chat_open || state.nav_tab == NavTab::Sessions;
  vis.compact_chat_overlay = state.compact_chat_open;
  vis.auxiliary_sheet = state.auxiliary_open;
  return vis;
}

CompactChromeLayout ShellLayout::ComputeCompactChromeLayout(const ShellConfig& config,
                                                            int safe_area_top_dp,
                                                            int safe_area_bottom_dp) {
  CompactChromeLayout layout{};
  const float safe_top = static_cast<float>(std::max(0, safe_area_top_dp));
  const float safe_bottom = static_cast<float>(std::max(0, safe_area_bottom_dp));
  // Shrink the shell to the safe rect (status bar / home indicator / IME). Chrome
  // sits at the bottom of that rect; content only needs nav-height padding.
  layout.shell_top_dp = safe_top;
  layout.shell_bottom_dp = safe_bottom;
  layout.content_padding_bottom_dp = config.compact_nav_height_dp;
  layout.chrome_bottom_dp = 0.f;
  layout.sheet_bottom_dp = config.compact_nav_height_dp;
  layout.chrome_horizontal_inset_dp = config.compact_nav_horizontal_inset_dp;
  return layout;
}

} // namespace pbr
