#pragma once

#include "foundation/platform/DeviceRinger.h"

#include <optional>
#include "common/PbrCompat.h"

namespace pbr {

/** What the app's own incoming ringtone does now. */
struct CallRingerOutput {
  bool sound = false;
  bool vibrate = false;
};

/**
 * The app's incoming ringtone follows the phone (DEV/PLAN-android-incoming-call-screen.md step 4): Do Not Disturb on
 * (any filter but "all") → nothing; silent → nothing; vibrate → vibration only; normal → sound, unless the ring volume is
 * 0 (nothing). An unknown mode, or a reading the platform could not give, is nothing.
 */
CallRingerOutput CallRingerOutputFor(const std::optional<DeviceRingerReading>& reading);

} // namespace pbr
