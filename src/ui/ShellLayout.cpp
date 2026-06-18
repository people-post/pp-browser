#include "ui/ShellLayout.h"

namespace pbr {

LayoutMode ShellLayout::FromWidth(float width_dp, float breakpoint) {
  return width_dp < breakpoint ? LayoutMode::Compact : LayoutMode::Expanded;
}

const char* ShellLayout::LayoutModeString(LayoutMode mode) {
  return mode == LayoutMode::Compact ? "compact" : "expanded";
}

void ShellLayout::SyncLayoutModeString(ShellState& state) {
  state.layout_mode_str = LayoutModeString(state.layout_mode);
}

PaneVisibility ShellLayout::WhichPanesVisible(const ShellState& state) {
  PaneVisibility vis{};
  vis.primary = true;

  if (state.layout_mode == LayoutMode::Expanded) {
    vis.secondary = true;
    vis.auxiliary = state.auxiliary_open;
    return vis;
  }

  vis.secondary_drawer = state.secondary_drawer_open;
  vis.auxiliary_sheet = state.auxiliary_open;
  return vis;
}

} // namespace pbr
