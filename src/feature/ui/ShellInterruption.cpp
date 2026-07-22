#include "feature/ui/ShellInterruption.h"

namespace pbr {

InterruptionKind ShellInterruption::Top(const ShellState& state) {
  if (state.pin_gate.active) {
    return InterruptionKind::PinGate;
  }
  if (state.dialog.active) {
    return InterruptionKind::Dialog;
  }
  if (!state.overlay_stack.empty()) {
    return InterruptionKind::OverlayLayer;
  }
  if (!state.transient_stack.empty()) {
    return InterruptionKind::Transient;
  }
  if (state.account_sheet_open) {
    return InterruptionKind::AccountSheet;
  }
  if (state.layout_mode == LayoutMode::Compact && state.auxiliary_open) {
    return InterruptionKind::AuxiliarySheet;
  }
  if (state.layout_mode == LayoutMode::Compact && state.compact_chat_open) {
    return InterruptionKind::CompactChatOverlay;
  }
  return InterruptionKind::None;
}

CompactChromeFrostSurface ShellInterruption::ResolveFrostSurface(const ShellState& state) {
  if (state.layout_mode != LayoutMode::Compact) {
    return CompactChromeFrostSurface::None;
  }
  switch (Top(state)) {
  case InterruptionKind::PinGate:
  case InterruptionKind::Dialog:
  case InterruptionKind::OverlayLayer:
    return CompactChromeFrostSurface::None;
  case InterruptionKind::Transient:
    return CompactChromeFrostSurface::TransientHeader;
  case InterruptionKind::AccountSheet:
    return CompactChromeFrostSurface::AccountSheetHeader;
  case InterruptionKind::AuxiliarySheet:
    return CompactChromeFrostSurface::AuxiliarySheetChrome;
  case InterruptionKind::CompactChatOverlay:
    return CompactChromeFrostSurface::ChatOverlayHeader;
  case InterruptionKind::None:
    return CompactChromeFrostSurface::BottomNav;
  }
  return CompactChromeFrostSurface::None;
}

bool ShellInterruption::DismissTop(ShellState& state) {
  switch (Top(state)) {
  case InterruptionKind::PinGate:
    // Handled in ShellHost::HandleDismiss (create cancels; unlock blocks).
    return false;
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
  case InterruptionKind::AccountSheet:
    state.account_sheet_open = false;
    return true;
  case InterruptionKind::AuxiliarySheet:
    state.auxiliary_open = false;
    return true;
  case InterruptionKind::CompactChatOverlay:
    state.compact_chat_open = false;
    return true;
  case InterruptionKind::None:
    return false;
  }
  return false;
}

} // namespace pbr
