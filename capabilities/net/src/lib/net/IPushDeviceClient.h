#pragma once

#include "common/Error.h"

#include <string>
#include "common/PbrCompat.h"

namespace pbr {

struct PushDeviceRegistration {
  std::string platform;
  std::string push_token;
  std::string device_id;
  std::string account_id;
};

/**
 * This device's push binding on one relay (`/v1/devices/register`, `unregister`). An unregister removes only the binding the last register
 * made (its `binding_id`, multi-relay MR007 item 5), so a late one leaves a newer binding alone.
 */
class IPushDeviceClient {
public:
  virtual ~IPushDeviceClient() = default;
  virtual Roe<void> RegisterDevice(const PushDeviceRegistration& registration) = 0;
  virtual Roe<void> UnregisterDevice(const PushDeviceRegistration& registration) = 0;
};

} // namespace pbr
