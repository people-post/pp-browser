#include "feature/ui/ShellInterruption.h"

namespace pbr {

InterruptionKind ShellInterruption::Top(const ShellState& state) {
  if (state.dialog.active) {
    return InterruptionKind::Dialog;
  }
  if (!state.overlay_stack.empty()) {
    return InterruptionKind::OverlayLayer;
  }
  if (!state.transient_stack.empty()) {
    return InterruptionKind::Transient;
  }
  if (state.layout_mode == LayoutMode::Compact && state.auxiliary_open) {
    return InterruptionKind::AuxiliarySheet;
  }
  if (state.layout_mode == LayoutMode::Compact && state.secondary_drawer_open) {
    return InterruptionKind::SecondaryDrawer;
  }
  return InterruptionKind::None;
}

bool ShellInterruption::DismissTop(ShellState& state) {
  switch (Top(state)) {
  case InterruptionKind::Dialog:
    state.dialog = {};
    state.transient_active = !state.transient_stack.empty();
    return true;
  case InterruptionKind::OverlayLayer:
    state.overlay_stack.pop_back();
    return true;
  case InterruptionKind::Transient:
    state.transient_stack.pop_back();
    state.transient_active = !state.transient_stack.empty();
    return true;
  case InterruptionKind::AuxiliarySheet:
    state.auxiliary_open = false;
    return true;
  case InterruptionKind::SecondaryDrawer:
    state.secondary_drawer_open = false;
    return true;
  case InterruptionKind::None:
    return false;
  }
  return false;
}

} // namespace pbr
