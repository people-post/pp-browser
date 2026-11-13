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
/**
 * The in-call chrome in its mode: Expanded (top bar), Immersive (people grid), Minimized (the chip: top right until it
 * is dragged, then docked on a side at its height — #166 — or tucked on the edge as a small handle, #189).
 */
std::string CallInProgressRml(const ShellState& state);
/**
 * #166: the top (dp) the minimised chip's markup gives a dropped chip: where the gesture last put it for this place
 * (kept in the band), else its share of the band with the heights last measured; < 0: no height known, no top.
 */
float MinimizedChipTopDp(const ShellState& state);
/** A ring or a live call is up: the frame loop runs often enough for its pulses and meters (kLiveChromeFrameIntervalSec). */
bool CallChromeWantsLiveFrames(const ShellState& state);

} // namespace pbr
