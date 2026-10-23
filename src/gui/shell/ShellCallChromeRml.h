#pragma once

#include "lib/ui/ShellTypes.h"

#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The call chrome's markup, bound to the window model's call_* fields: what ShellHost mounts into
 * #shell-call-ring-mount and #shell-call-in-progress-mount. Empty when that chrome is not up.
 */
std::string CallRingRml(const ShellState& state);
/** The in-call chrome in its mode: Expanded (top bar), Immersive (people grid), Minimized (corner chip). */
std::string CallInProgressRml(const ShellState& state);

} // namespace pbr
