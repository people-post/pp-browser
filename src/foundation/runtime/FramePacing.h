#pragma once

namespace pbr {

/**
 * How often the app runs a frame when nothing wakes it (a posted UI task, input or a timer wakes it
 * at once). Each frame ticks the controllers (CallController::Tick: expired rings, the in-call
 * screen), so these are also their cadence. pp-app-probe paces its frames the same way.
 */
/** While a ring or a call panel shows: the ring pulse and in-call meters stay live. */
inline constexpr double kCallChromeFrameIntervalSec = 0.55;
/** Idle (power save): the longest wait between frames. */
inline constexpr double kIdleFrameMaxWaitSec = 2.0;

} // namespace pbr
