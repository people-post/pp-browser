#pragma once

#include "common/PbrCompat.h"

namespace pbr {

/**
 * Raise the screen to full brightness (`raised` true) so a QR code on it is easy for another phone to read, and give
 * back the brightness the user had (`raised` false). Remembers the value from the first raise; a second raise, or a
 * restore with nothing raised, does nothing. Nothing is stored on disk: the caller restores on every way the QR page
 * goes away (it closes, the app leaves the foreground).
 *
 * iOS only (UIScreen.main.brightness); a no-op elsewhere (Android would need a call on the activity's UI thread, which
 * the JNI pattern here cannot make without a Java change). UI thread only.
 */
void SetScreenBrightnessMax(bool raised);

} // namespace pbr
