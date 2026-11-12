#pragma once

#include <optional>
#include "common/PbrCompat.h"

namespace pbr {

/** Whether the phone's screen is on and unlocked: an app in front on a dark or locked phone is not seen. */
struct DeviceScreenState {
  /** Android: PowerManager.isInteractive(). */
  bool interactive = true;
  /** Android: KeyguardManager.isKeyguardLocked(). */
  bool keyguard_locked = false;
};

/**
 * Asked of the OS on every call (nothing kept: a ring hand-over decides by it, DEV/PLAN-android-incoming-call-screen.md).
 * Nothing: the OS could not be asked (no activity, no such method, it threw) — the caller treats that as not seen.
 * Android only; elsewhere always on and unlocked. UI thread.
 */
std::optional<DeviceScreenState> ReadDeviceScreenState();

} // namespace pbr
