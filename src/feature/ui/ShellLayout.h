#pragma once

#include "ui/ShellTypes.h"

namespace pbr {

struct ShellLayout {
  static LayoutMode FromWidth(float width_dp, float breakpoint = ShellConfig{}.compact_breakpoint_dp);
  static const char* LayoutModeString(LayoutMode mode);
  static PaneVisibility WhichPanesVisible(const ShellState& state);
  static void SyncLayoutModeString(ShellState& state);
};

} // namespace pbr
