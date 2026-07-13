#pragma once

#include "common/Error.h"

#include <string>

namespace pbr {

/** Platform push token source (FCM on Android; unsupported on desktop). */
class IPushDeviceRegistrar {
public:
  virtual ~IPushDeviceRegistrar() = default;

  virtual bool IsSupported() const = 0;
  /** Empty error message means unsupported / unavailable. */
  virtual Roe<std::string> GetPushToken() = 0;
  virtual std::string PlatformName() const = 0;
  virtual std::string DeviceId() const = 0;

  static IPushDeviceRegistrar& Instance();
  static void SetInstance(IPushDeviceRegistrar* registrar);
};

} // namespace pbr
