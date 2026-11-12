#include "foundation/platform/DeviceRinger.h"

namespace pbr {

std::optional<DeviceRingerReading> ReadDeviceRinger() {
  return DeviceRingerReading{};
}

void SetDeviceRingVibration(const bool on) {
  (void)on;
}

} // namespace pbr
