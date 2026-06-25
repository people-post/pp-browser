#pragma once

#include "base/ui/ShellTypes.h"

namespace pbr {

struct ShellInterruption {
  static InterruptionKind Top(const ShellState& state);
  static bool DismissTop(ShellState& state);
};

} // namespace pbr
