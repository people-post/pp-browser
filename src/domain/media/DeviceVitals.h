#pragma once

namespace pbr {

/** Device load for operational metrics (Metrics.h): what a call costs the device. -1 = unknown. */
struct DeviceVitals {
  /** CPU time this process has used so far (user + system), seconds — the power-use proxy. */
  double cpu_s = -1.0;
  /** 0 nominal, 1 fair, 2 serious, 3 critical (Apple thermal state). */
  int thermal = -1;
  /** Battery charge, percent. */
  int battery_pct = -1;
  bool charging = false;
};

/** UI thread (iOS reads UIDevice). Cheap enough for once every couple of seconds. */
DeviceVitals ReadDeviceVitals();

inline const char* ThermalStateName(const int thermal) {
  switch (thermal) {
  case 0:
    return "nominal";
  case 1:
    return "fair";
  case 2:
    return "serious";
  case 3:
    return "critical";
  default:
    return "unknown";
  }
}

} // namespace pbr
