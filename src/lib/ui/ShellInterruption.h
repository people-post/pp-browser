#pragma once

#include "lib/ui/ShellTypes.h"

namespace pbr {

struct ShellInterruption {
  static InterruptionKind Top(const ShellState& state);
  static bool DismissTop(ShellState& state);
  /**
   * Which compact chrome bars may use backdrop frost: at most one per bar position (top, bottom), so two blurs per
   * frame at most (the chat view: header + composer). Both None when modals cover the chrome. The tab page's
   * BottomNav counts as both of its bars (nav bar on top, tab bar below), as before.
   */
  static CompactChromeFrost ResolveFrost(const ShellState& state);
  /** The top bar's surface (ResolveFrost(state).top): the one bar a page without a bottom frost declares. */
  static CompactChromeFrostSurface ResolveFrostSurface(const ShellState& state);
};

} // namespace pbr
