#pragma once

#include <functional>

namespace pbr {

/** The OS permission to record, asked about without opening the microphone. */
enum class MicPermission { Granted, Denied, NotDetermined };

/**
 * Where the OS has this app's microphone permission. Windows and Linux have no per-app prompt the
 * app can ask about: they answer Granted (a blocked microphone shows up as an open that fails).
 */
MicPermission MicPermissionState();

/**
 * Show the OS prompt when the answer is not known yet; `done(granted)` runs once, on any thread, and
 * at once (before this returns) when there is nothing to ask. Never opens the microphone.
 */
void RequestMicPermission(std::function<void(bool granted)> done);

} // namespace pbr
