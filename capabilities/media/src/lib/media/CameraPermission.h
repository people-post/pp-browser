#pragma once

#include <functional>

namespace pbr {

/** The OS permission to use the camera, asked about without opening it. */
enum class CameraPermission { Granted, Denied, NotDetermined };

/**
 * Where the OS has this app's camera permission. Desktops answer Granted: they have no prompt the app asks for ahead
 * of time (macOS shows its own when the camera opens; a blocked camera shows up as an open that fails).
 */
CameraPermission CameraPermissionState();

/**
 * Show the OS prompt when the answer is not known yet; `done(granted)` runs once, on any thread, and at once (before
 * this returns) when there is nothing to ask. Never opens the camera.
 */
void RequestCameraPermission(std::function<void(bool granted)> done);

} // namespace pbr
