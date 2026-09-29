#include "domain/media/DeviceVitals.h"

#include <ctime>

namespace pbr {

DeviceVitals ReadDeviceVitals() {
  DeviceVitals v;
  const std::clock_t cpu = std::clock();
  if (cpu != static_cast<std::clock_t>(-1)) {
    v.cpu_s = static_cast<double>(cpu) / CLOCKS_PER_SEC;
  }
  return v;
}

} // namespace pbr
