#include "domain/media/DeviceVitals.h"

#include <TargetConditionals.h>
#import <Foundation/Foundation.h>
#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#endif

#include <sys/resource.h>

namespace pbr {

DeviceVitals ReadDeviceVitals() {
  DeviceVitals v;
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) == 0) {
    v.cpu_s = static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) +
              static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
  }
  switch ([NSProcessInfo processInfo].thermalState) {
  case NSProcessInfoThermalStateNominal:
    v.thermal = 0;
    break;
  case NSProcessInfoThermalStateFair:
    v.thermal = 1;
    break;
  case NSProcessInfoThermalStateSerious:
    v.thermal = 2;
    break;
  case NSProcessInfoThermalStateCritical:
    v.thermal = 3;
    break;
  }
#if TARGET_OS_IPHONE
  UIDevice* device = [UIDevice currentDevice];
  if (!device.batteryMonitoringEnabled) {
    device.batteryMonitoringEnabled = YES;
  }
  if (device.batteryLevel >= 0) {
    v.battery_pct = static_cast<int>(device.batteryLevel * 100.0f + 0.5f);
  }
  v.charging = device.batteryState == UIDeviceBatteryStateCharging || device.batteryState == UIDeviceBatteryStateFull;
#endif
  return v;
}

} // namespace pbr
