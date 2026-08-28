#pragma once

#include "base/platform/IPushDeviceRegistrar.h"
#include "common/PbrCompat.h"

namespace pbr {

class AndroidPushDeviceRegistrar final : public IPushDeviceRegistrar {
public:
  bool IsSupported() const override;
  Roe<std::string> GetPushToken() override;
  std::string PlatformName() const override { return "android"; }
  std::string DeviceId() const override;

  void SetCachedToken(std::string token);
};

} // namespace pbr
