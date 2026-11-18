#pragma once

#include <functional>
#include <string>

namespace pbr {

/**
 * Starts reporting the on-screen keyboard on platforms whose safe area does not include it (iOS).
 * Returns false elsewhere (Android publishes the keyboard through the safe area; desktops have none), and
 * `on_change` is then never called. `on_change` runs on the UI thread each time the height changes.
 */
bool WatchScreenKeyboard(std::function<void()> on_change);

/** Height the on-screen keyboard covers at the bottom of our window, in window points; 0 when hidden or floating. */
int ScreenKeyboardHeight();

/** iPad: the keyboard notifications seen since the last call, comma-separated (for one log line each time); "" when
 *  none and on other devices. */
std::string TakeScreenKeyboardEvents();

/** One line for the log: the keyboard frame as reported, as placed in our window, and our window (iOS); "" elsewhere. */
std::string DescribeScreenKeyboard();

} // namespace pbr
