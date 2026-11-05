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
/**
 * While a call's video tile shows (self-view or a peer): the camera's rate. Nothing else wakes the
 * frame loop when a video frame arrives — the engine only leaves it for the next frame to copy — so
 * without this the self-view and the peer were drawn whenever something unrelated woke the loop
 * (0.55 s at most; meters at most every 200 ms, and none in video mode), and both looked choppy.
 */
inline constexpr double kVideoFrameIntervalSec = 1.0 / 30.0;

} // namespace pbr
