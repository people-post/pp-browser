#include "lib/ui/ShellLayout.h"

#include <algorithm>

namespace pbr {

LayoutMode ShellLayout::FromWidth(float width_dp, float breakpoint) {
  return width_dp < breakpoint ? LayoutMode::Compact : LayoutMode::Expanded;
}

const char* ShellLayout::LayoutModeString(LayoutMode mode) {
  return mode == LayoutMode::Compact ? "compact" : "expanded";
}

const char* ShellLayout::NavTabString(NavTab tab) {
  if (tab == NavTab::Contacts) {
    return "contacts";
  }
  if (tab == NavTab::Sessions) {
    return "sessions";
  }
  if (tab == NavTab::Me) {
    return "me";
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
  if (tab == NavTab::Contacts) {
    return "contacts";
  }
  if (tab == NavTab::Me) {
    return "settings";
  }
  return "sidebar";
}

bool ShellLayout::TabHasSecondary(NavTab tab) {
  return NavContentKey(tab) != nullptr;
}

int ShellLayout::ClampSidebarWidthDp(int width_dp) {
  return std::clamp(width_dp, kSidebarMinWidthDp, kSidebarMaxWidthDp);
}

int ShellLayout::ClampAuxiliaryWidthDp(int width_dp) {
  return std::clamp(width_dp, kAuxiliaryMinWidthDp, kAuxiliaryMaxWidthDp);
}

bool ShellLayout::SecondaryPaneShown(const ShellState& state) {
  return state.layout_mode == LayoutMode::Expanded && TabHasSecondary(state.nav_tab) && !state.sidebar_collapsed;
}

bool ShellLayout::SidebarCollapsedAfterNavSelect(bool collapsed, NavTab tab) {
  return TabHasSecondary(tab) ? false : collapsed;
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
                                                            int safe_area_bottom_dp,
                                                            float titlebar_height_dp,
                                                            CompactBottomSurface bottom_surface) {
  CompactChromeLayout layout{};
  const float safe_top = static_cast<float>(std::max(0, safe_area_top_dp));
  const float safe_bottom = static_cast<float>(std::max(0, safe_area_bottom_dp));
  const float titlebar = std::max(0.f, titlebar_height_dp);
  // Edge-to-edge at the top: body stays at top:0 so theme background shows under
  // the status bar; content_top_dp insets #shell-root / chrome instead.
  // Desktop adds the custom title bar height into content_top_dp.
  // Bottom still shrinks the body for home indicator / IME; chrome sits at the
  // bottom of that rect and content only needs nav-height padding.
  layout.shell_top_dp = 0.f;
  layout.content_top_dp = safe_top + titlebar;
  // Without a keyboard, the bottom surface paints down through the home-indicator strip: the document
  // goes full-bleed and the surface pads itself by the inset. With a keyboard up the body shrinks.
  const bool keyboard = safe_area_bottom_dp >= kImeLatchMinDp;
  const bool covers_strip = bottom_surface != CompactBottomSurface::None && !keyboard;
  layout.shell_bottom_dp = covers_strip ? 0.f : safe_bottom;
  layout.bottom_surface_padding_dp = covers_strip ? safe_bottom : 0.f;
  const float tab_bar_inset =
      bottom_surface == CompactBottomSurface::TabBar ? layout.bottom_surface_padding_dp : 0.f;
  layout.content_padding_bottom_dp = config.compact_nav_height_dp + tab_bar_inset;
  layout.chrome_bottom_dp = 0.f;
  layout.sheet_bottom_dp = config.compact_nav_height_dp + tab_bar_inset;
  layout.chrome_horizontal_inset_dp = config.compact_nav_horizontal_inset_dp;
  if (bottom_surface != CompactBottomSurface::None) {
    // The bar the toast must clear: tab bar, or the composer's one-line height (DESIGN-pp-ui 8.5); a pushed
    // page has none. Below it the home-indicator strip (with a keyboard up the body already ends above it).
    constexpr float kComposerMinHeightDp = 52.f;
    constexpr float kToastGapDp = 12.f;
    const float bar = bottom_surface == CompactBottomSurface::TabBar     ? config.compact_nav_height_dp
                      : bottom_surface == CompactBottomSurface::Composer ? kComposerMinHeightDp
                                                                         : 0.f;
    layout.toast_bottom_dp = layout.bottom_surface_padding_dp + bar + kToastGapDp;
  }
  return layout;
}

} // namespace pbr
