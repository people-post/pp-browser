#include "base/platform/IPushDeviceRegistrar.h"

namespace pbr {

namespace {

class UnsupportedPushDeviceRegistrar final : public IPushDeviceRegistrar {
public:
  bool IsSupported() const override { return false; }
  Roe<std::string> GetPushToken() override { return Error("Push tokens not supported on this platform"); }
  std::string PlatformName() const override { return "desktop"; }
  std::string DeviceId() const override { return "desktop"; }
};

IPushDeviceRegistrar* g_push_registrar = nullptr;
UnsupportedPushDeviceRegistrar g_unsupported_push_registrar;

} // namespace

IPushDeviceRegistrar& IPushDeviceRegistrar::Instance() {
  return g_push_registrar ? *g_push_registrar : g_unsupported_push_registrar;
}

void IPushDeviceRegistrar::SetInstance(IPushDeviceRegistrar* registrar) {
  g_push_registrar = registrar;
}

} // namespace pbr
