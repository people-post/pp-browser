#pragma once

namespace pbr {

/**
 * Sets the number on the app's icon (iOS home screen, macOS Dock). `count` <= 0 clears it. Any thread: the
 * platform switches to the thread it needs. Android sets the vivo launcher's number through MainActivity (other
 * launchers follow the notifications); Windows and Linux have nothing to set.
 */
void SetAppBadgeCount(int count);

} // namespace pbr
