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
};

} // namespace pbr
