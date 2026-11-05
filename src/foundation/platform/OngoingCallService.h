#pragma once

#include <functional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The call this device is in, to the OS. On Android a foreground service of the microphone type with its
 * ongoing-call notification (the caller's name, Hang up): without it an app in the background records silence
 * (Android 9+), and its process may go. Started when the call starts — the app is in front then, as Android 12+
 * requires — and stopped when it ends. Elsewhere nothing (iOS keeps a call's audio by its background mode).
 *
 * UI thread.
 */
namespace OngoingCallService {

/** In a call (`title`: who with, for the notification), or not. Repeating the same state does nothing. */
void SetOngoingCall(bool on, const std::string& title);
/** The user hung up on the notification: `fn` runs on the UI thread. Empty clears it. */
void SetHangUpHandler(std::function<void()> fn);

} // namespace OngoingCallService

} // namespace pbr
