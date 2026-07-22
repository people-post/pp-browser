#pragma once

#include "base/ui/ShellTypes.h"

namespace pbr {

struct ShellInterruption {
  static InterruptionKind Top(const ShellState& state);
  static bool DismissTop(ShellState& state);
  /** Which compact chrome bar may use backdrop frost (at most one; None when modals cover chrome). */
  static CompactChromeFrostSurface CompactChromeFrostSurface(const ShellState& state);
};

} // namespace pbr
