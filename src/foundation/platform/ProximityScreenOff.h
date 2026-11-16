#pragma once

#include "common/PbrCompat.h"

namespace pbr {

/**
 * Let the phone black the screen and stop touches when it is held to the ear (the proximity sensor), as a voice call's
 * earpiece use needs. `want` says a voice call is on; the platform then decides from the real audio output route and
 * turns the sensor on only while the sound comes from the earpiece (not the speaker, headphones or Bluetooth). Calling
 * it again re-reads the route (a plugged-in headphone, a connected Bluetooth), so the caller repeats it while `want`
 * holds; it writes to the system only when the result changes. `false` always turns the sensor off.
 *
 * iOS only (UIDevice.proximityMonitoringEnabled); a no-op elsewhere. UI thread only.
 */
void SetProximityScreenOff(bool want);

} // namespace pbr
