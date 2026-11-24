#include "lib/media/DeviceVitals.h"

#include "lib/media/MediaHost.h"

#include <sys/resource.h>

namespace pbr {

DeviceVitals ReadDeviceVitals() {
  DeviceVitals v;
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) == 0) {
    v.cpu_s = static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) +
              static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
  }
  // Power saver, thermal state, battery: the host's activity reads them (MediaHost::read_device_vitals).
  const MediaHost host = CurrentMediaHost();
  if (host.read_device_vitals) {
    if (const std::optional<DeviceVitals> from_host = host.read_device_vitals()) {
      v.low_power = from_host->low_power;
      v.thermal = from_host->thermal;
      v.battery_pct = from_host->battery_pct;
      v.charging = from_host->charging;
    }
  }
  return v;
}

} // namespace pbr
