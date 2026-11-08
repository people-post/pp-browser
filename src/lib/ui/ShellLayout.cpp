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

bool ShellLayout::IsHiddenTabOnCompact(const NavTab tab) {
  return tab == NavTab::Me || tab == NavTab::Contacts;
}

bool ShellLayout::HiddenTabShown(const ShellState& state) {
  return state.layout_mode == LayoutMode::Compact && IsHiddenTabOnCompact(state.nav_tab);
}

float ShellLayout::CompactTabBarDp(const ShellState& state, const float bar_height_dp) {
  // A pushed page covers the bar but does not remove it: during its swipe-back the page underneath shows its bar.
  if (state.layout_mode != LayoutMode::Compact || HiddenTabShown(state) || state.compact_chat_open) {
    return 0.f;
  }
  return bar_height_dp;
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

bool ShellLayout::OpenChatSwitchesToChats(const LayoutMode mode, const NavTab tab) {
  return mode == LayoutMode::Compact && tab == NavTab::Home;
}

PaneVisibility ShellLayout::WhichPanesVisible(const ShellState& state) {
  PaneVisibility vis{};
  vis.primary = true;

  if (state.layout_mode == LayoutMode::Expanded) {
    vis.secondary = true;
    vis.auxiliary = state.auxiliary_open;
    return vis;
  }

  // A hidden tab's page (Me, Contacts) is drawn over a chat overlay left open under it; the overlay comes back with
  // its chat on the way back (hidden_tab_back).
  const bool hidden_tab = HiddenTabShown(state);
  vis.compact_nav_page = !state.compact_chat_open || state.nav_tab == NavTab::Sessions || hidden_tab;
  vis.compact_chat_overlay = state.compact_chat_open && !hidden_tab;
  vis.auxiliary_sheet = state.auxiliary_open;
  return vis;
}

CompactChromeLayout ShellLayout::ComputeCompactChromeLayout(const ShellConfig& config,
                                                            int safe_area_top_dp,
                                                            int safe_area_bottom_dp,
                                                            float titlebar_height_dp,
                                                            CompactBottomSurface bottom_surface,
                                                            bool bottom_chrome_open) {
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
  // Without a keyboard, the bottom surface paints down through the home-indicator strip: the document goes full-bleed
  // and the surface pads itself by the inset. With a keyboard up the body still goes full-bleed (so the theme background
  // paints behind the keyboard, not the window's black clear colour) and the keyboard inset lifts #shell-root instead.
  const bool keyboard = safe_area_bottom_dp >= kImeLatchMinDp;
  const bool covers_strip = bottom_surface != CompactBottomSurface::None && !keyboard;
  layout.shell_bottom_dp = (covers_strip || keyboard) ? 0.f : safe_bottom;
  layout.content_bottom_inset_dp = keyboard ? safe_bottom : 0.f;
  layout.bottom_surface_padding_dp = covers_strip ? safe_bottom : 0.f;
  const float tab_bar_inset =
      bottom_surface == CompactBottomSurface::TabBar ? layout.bottom_surface_padding_dp : 0.f;
  // Keyboard up, or an emoji/attachment bottom sheet open, over a tab-bar page: hide the tab bar and stop reserving its
  // height, so the content (and its docked composer) sits right above the keyboard / panel that covers where the bar
  // was, instead of being squeezed above a floating bar (Kenneth 2026-11-05). bottom_chrome_open is the emoji/attachment
  // panel, whose own inset is zeroed at the call site, so it is passed in separately from the keyboard inset.
  layout.tab_bar_hidden = (keyboard || bottom_chrome_open) && bottom_surface == CompactBottomSurface::TabBar;
  layout.content_padding_bottom_dp = layout.tab_bar_hidden ? 0.f : config.compact_nav_height_dp + tab_bar_inset;
  layout.chrome_bottom_dp = 0.f;
  layout.sheet_bottom_dp = config.compact_nav_height_dp + tab_bar_inset;
  layout.chrome_horizontal_inset_dp = config.compact_nav_horizontal_inset_dp;
  if (bottom_surface != CompactBottomSurface::None) {
    // The bar the toast must clear: tab bar, or the composer's one-line height (DESIGN-pp-ui 8.5); a pushed
    // page has none. A hidden tab bar is not there to clear. The body no longer shrinks for the keyboard, so the
    // toast (anchored to the body bottom) adds the keyboard inset to stay above it.
    constexpr float kComposerMinHeightDp = 52.f;
    constexpr float kToastGapDp = 12.f;
    const float bar = layout.tab_bar_hidden                              ? 0.f
                      : bottom_surface == CompactBottomSurface::TabBar     ? config.compact_nav_height_dp
                      : bottom_surface == CompactBottomSurface::Composer ? kComposerMinHeightDp
                                                                         : 0.f;
    layout.toast_bottom_dp = layout.content_bottom_inset_dp + layout.bottom_surface_padding_dp + bar + kToastGapDp;
  }
  return layout;
}

bool ShellLayout::PinGatePayloadTooBigToRender(bool phrase_flow, size_t payload_len) {
  // A normal 24-word recovery phrase is well under this; anything larger is a transfer code dropped in the wrong box.
  constexpr size_t kPinGateRenderMax = 512;
  return (!phrase_flow && payload_len > 0) || payload_len > kPinGateRenderMax;
}

} // namespace pbr
