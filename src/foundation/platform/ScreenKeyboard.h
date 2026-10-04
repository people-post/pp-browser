#pragma once

#include <functional>

namespace pbr {

/**
 * Starts reporting the on-screen keyboard on platforms whose safe area does not include it (iOS).
 * Returns false elsewhere (Android publishes the keyboard through the safe area; desktops have none), and
 * `on_change` is then never called. `on_change` runs on the UI thread each time the height changes.
 */
bool WatchScreenKeyboard(std::function<void()> on_change);

/** Height the on-screen keyboard covers at the bottom of the screen, in window points; 0 when hidden. */
int ScreenKeyboardHeight();

} // namespace pbr
