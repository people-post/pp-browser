#include "foundation/platform/ProximityScreenOff.h"

#include "common/Logger.h"

#import <AVFoundation/AVFoundation.h>
#import <UIKit/UIKit.h>

namespace pbr {

namespace {

bool OutputIsReceiver() {
  for (AVAudioSessionPortDescription* output in AVAudioSession.sharedInstance.currentRoute.outputs) {
    if ([output.portType isEqualToString:AVAudioSessionPortBuiltInReceiver]) {
      return true;
    }
  }
  return false;
}

} // namespace

void SetProximityScreenOff(const bool want) {
  @autoreleasepool {
    const bool receiver = want && OutputIsReceiver();
    UIDevice* device = UIDevice.currentDevice;
    if (device.proximityMonitoringEnabled == (receiver ? YES : NO)) {
      return;
    }
    device.proximityMonitoringEnabled = receiver ? YES : NO;
    logging::getLogger("ProximityScreenOff").info << "proximity screen-off " << (receiver ? "on" : "off")
                                                  << " route=" << (receiver ? "receiver" : "other");
  }
}

} // namespace pbr
