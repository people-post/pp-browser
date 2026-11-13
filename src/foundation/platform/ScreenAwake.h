#pragma once

#include "common/PbrCompat.h"

namespace pbr {

/**
 * Keep the screen on, or let the OS sleep it (the user's auto-lock). PP lets it sleep (BrowserHost allows the screen
 * saver before SDL_Init); a video call holds it on. A thin wrapper over SDL's switch: iOS idle timer, Android
 * FLAG_KEEP_SCREEN_ON, a Windows / macOS / Linux display-sleep block.
 *
 * UI thread only (SDL's main thread; the call controller's tick and the window bring-up both run there, on Android
 * too: SDL_main's thread posts the flag to the activity itself).
 */
namespace ScreenAwake {

/**
 * `reason` is a short word for the log. Logs the switch and what the OS reports afterwards
 * (`screen awake on|off reason=… allowed_by_os=0|1`); a failed switch logs a warning.
 */
void SetKeepScreenOn(bool on, const char* reason);

/** Logs what the OS reports now (`screen awake <reason> allowed_by_os=0|1`), without changing anything. */
void LogOsState(const char* reason);

} // namespace ScreenAwake

} // namespace pbr
