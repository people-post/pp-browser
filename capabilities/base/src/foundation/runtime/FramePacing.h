#pragma once

namespace pbr {

/**
 * How often the app runs a frame when nothing wakes it (a posted UI task, input or a timer wakes it
 * at once). Each frame ticks the controllers (e.g. the call screen's expired rings and meters), so
 * these are also their cadence. pp-app-probe paces its frames the same way.
 */
/** While live chrome shows (a ring, a call panel): its pulse and meters stay live. */
inline constexpr double kLiveChromeFrameIntervalSec = 0.55;
/** Idle (power save): the longest wait between frames. */
inline constexpr double kIdleFrameMaxWaitSec = 2.0;

} // namespace pbr
