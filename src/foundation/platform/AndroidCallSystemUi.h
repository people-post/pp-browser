#pragma once

#include "common/PbrCompat.h"

namespace pbr {

/**
 * The phone's own call screen on Android: an incoming-call notification (full screen on a locked phone, Answer /
 * Decline) posted by PpCallNotifications.java. That class also holds the push's call (a call push that reached the
 * phone before the app found the ring), since a push can come while the app's native side is not running at all.
 * Installed by PlatformHooks::Register.
 */
void InstallAndroidCallSystemUi();

} // namespace pbr
