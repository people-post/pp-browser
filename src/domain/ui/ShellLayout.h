#pragma once

#include "domain/ui/ShellTypes.h"

namespace pbr {

struct ShellLayout {
  static LayoutMode FromWidth(float width_dp, float breakpoint = ShellConfig{}.compact_breakpoint_dp);
  static const char* LayoutModeString(LayoutMode mode);
  static const char* NavTabString(NavTab tab);
  static void SyncLayoutModeString(ShellState& state);
  static void SyncNavTabString(ShellState& state);
  static PaneVisibility WhichPanesVisible(const ShellState& state);
  static const char* NavContentKey(NavTab tab);
  static bool TabHasSecondary(NavTab tab);

  static constexpr int kSidebarMinWidthDp = 200;
  static constexpr int kSidebarMaxWidthDp = 480;
  static constexpr int kSidebarDefaultWidthDp = 240;
  static int ClampSidebarWidthDp(int width_dp);
  static constexpr int kAuxiliaryMinWidthDp = 280;
  static constexpr int kAuxiliaryMaxWidthDp = 640;
  static constexpr int kAuxiliaryDefaultWidthDp = 320;
  static int ClampAuxiliaryWidthDp(int width_dp);
  /** Expanded layout, tab has a secondary pane, and the user has not collapsed it. */
  static bool SecondaryPaneShown(const ShellState& state);
  /** Collapsed flag after the user picks `tab` in the nav rail: tabs with a secondary pane re-expand it. */
  static bool SidebarCollapsedAfterNavSelect(bool collapsed, NavTab tab);
  static CompactChromeLayout ComputeCompactChromeLayout(const ShellConfig& config,
                                                        int safe_area_top_dp,
                                                        int safe_area_bottom_dp,
                                                        float titlebar_height_dp = 0.f);
};

} // namespace pbr
