#pragma once

#include <optional>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * What the phone's ringer is set to, in Android's own values (the app's incoming ringtone follows it,
 * DEV/PLAN-android-incoming-call-screen.md step 4). Elsewhere: normal, nothing filtered, audible.
 */
struct DeviceRingerReading {
  static constexpr int kModeSilent = 0;  // AudioManager.RINGER_MODE_SILENT
  static constexpr int kModeVibrate = 1; // AudioManager.RINGER_MODE_VIBRATE
  static constexpr int kModeNormal = 2;  // AudioManager.RINGER_MODE_NORMAL
  /** NotificationManager.INTERRUPTION_FILTER_ALL: Do Not Disturb is off. */
  static constexpr int kFilterAll = 1;

  /** AudioManager.getRingerMode(). */
  int ringer_mode = kModeNormal;
  /** NotificationManager.getCurrentInterruptionFilter(). */
  int interruption_filter = kFilterAll;
  /** AudioManager.getStreamVolume(STREAM_RING). */
  int ring_volume = 1;
};

/** Asked of the OS on every call. Nothing: it could not be asked. UI thread. */
std::optional<DeviceRingerReading> ReadDeviceRinger();

/** The phone's incoming-call vibration (a repeating pattern) on or off. Android only; nothing elsewhere. UI thread. */
void SetDeviceRingVibration(bool on);

} // namespace pbr
