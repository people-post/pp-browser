#pragma once

#include "foundation/platform/IPushDeviceRegistrar.h"
#include "common/PbrCompat.h"

namespace pbr {

class IosPushDeviceRegistrar final : public IPushDeviceRegistrar {
public:
  bool IsSupported() const override;
  Roe<std::string> GetPushToken() override;
  std::string PlatformName() const override { return "ios"; }
  std::string DeviceId() const override;

  void SetCachedToken(std::string token);
};

/** Cache APNs device token (hex) and notify listeners. Safe from any thread. */
void SetIosCachedApnsToken(std::string token);

} // namespace pbr
