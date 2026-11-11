#include "feature/presenters/CallRingerOutput.h"

namespace pbr {

CallRingerOutput CallRingerOutputFor(const std::optional<DeviceRingerReading>& reading) {
  if (!reading || reading->interruption_filter != DeviceRingerReading::kFilterAll) {
    return {};
  }
  switch (reading->ringer_mode) {
    case DeviceRingerReading::kModeVibrate:
      return {.sound = false, .vibrate = true};
    case DeviceRingerReading::kModeNormal:
      return {.sound = reading->ring_volume > 0, .vibrate = false};
    default:
      return {};
  }
}

} // namespace pbr
