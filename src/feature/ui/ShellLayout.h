#pragma once

#include "base/ui/ShellTypes.h"

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
  static CompactChromeLayout ComputeCompactChromeLayout(const ShellConfig& config,
                                                        int safe_area_top_dp,
                                                        int safe_area_bottom_dp,
                                                        float titlebar_height_dp = 0.f);
};

} // namespace pbr
