#include "lib/ui/ShellInterruption.h"

#include "lib/ui/ShellLayout.h"

namespace pbr {

InterruptionKind ShellInterruption::Top(const ShellState& state) {
  if (state.pin_gate.active) {
    return InterruptionKind::PinGate;
  }
  if (state.call_ring.active) {
    return InterruptionKind::CallRing;
  }
  if (state.call_in_progress.active) {
    return InterruptionKind::LiveSessionOn;
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
  if (state.layout_mode == LayoutMode::Compact && state.auxiliary_open) {
    return InterruptionKind::AuxiliarySheet;
  }
  // Under a hidden tab's page (phone Me / Contacts) the chat overlay waits: that page is on top, not the overlay.
  if (state.layout_mode == LayoutMode::Compact && state.compact_chat_open && !ShellLayout::HiddenTabShown(state)) {
    return InterruptionKind::CompactChatOverlay;
  }
  return InterruptionKind::None;
}

CompactChromeFrost ShellInterruption::ResolveFrost(const ShellState& state) {
  using Surface = CompactChromeFrostSurface;
  if (state.layout_mode != LayoutMode::Compact) {
    return {};
  }
  switch (Top(state)) {
  case InterruptionKind::PinGate:
  case InterruptionKind::CallRing:
  case InterruptionKind::Dialog:
  case InterruptionKind::OverlayLayer:
    return {};
  case InterruptionKind::LiveSessionOn:
    // Compact call bar sits above chrome without a full-screen frost.
    return {};
  case InterruptionKind::Transient:
    // A page pushed from Settings keeps the tab bar under it: that bottom position is the bar's, the top the page's.
    if (ShellLayout::SettingsTabBarShown(state)) {
      return {.top = Surface::TransientHeader, .bottom = Surface::BottomNav};
    }
    return {.top = Surface::TransientHeader};
  case InterruptionKind::AuxiliarySheet:
    return {.top = Surface::AuxiliarySheetChrome};
  case InterruptionKind::CompactChatOverlay:
    // The chat view floats a header and a composer over the transcript: one blur each, nothing else.
    return {.top = Surface::ChatOverlayHeader, .bottom = Surface::ChatComposer};
  case InterruptionKind::None:
    return {.top = Surface::BottomNav, .bottom = Surface::BottomNav};
  }
  return {};
}

CompactChromeFrostSurface ShellInterruption::ResolveFrostSurface(const ShellState& state) {
  return ResolveFrost(state).top;
}

bool ShellInterruption::DismissTop(ShellState& state) {
  switch (Top(state)) {
  case InterruptionKind::PinGate:
    // Handled in ShellHost::HandleDismiss (create cancels; unlock blocks).
    return false;
  case InterruptionKind::CallRing:
    // Decline is explicit; Escape does not silently dismiss a ring.
    return false;
  case InterruptionKind::LiveSessionOn:
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
