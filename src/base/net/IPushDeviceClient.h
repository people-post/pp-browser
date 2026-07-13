#pragma once

#include "common/Error.h"

#include <string>

namespace pbr {

struct PushDeviceRegistration {
  std::string platform;
  std::string push_token;
  std::string device_id;
  std::string relay_user_id;
};

class IPushDeviceClient {
public:
  virtual ~IPushDeviceClient() = default;
  virtual Roe<void> RegisterDevice(const PushDeviceRegistration& registration) = 0;
  virtual Roe<void> UnregisterDevice(const PushDeviceRegistration& registration) = 0;
};

} // namespace pbr
