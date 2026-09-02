#include "foundation/platform/IPushDeviceRegistrar.h"

#include <mutex>
#include "common/PbrCompat.h"

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
std::mutex g_token_handler_mutex;
IPushDeviceRegistrar::TokenChangedFn g_token_changed_handler;

} // namespace

IPushDeviceRegistrar& IPushDeviceRegistrar::Instance() {
  return g_push_registrar ? *g_push_registrar : g_unsupported_push_registrar;
}

void IPushDeviceRegistrar::SetInstance(IPushDeviceRegistrar* registrar) {
  g_push_registrar = registrar;
}

void IPushDeviceRegistrar::SetTokenChangedHandler(TokenChangedFn handler) {
  std::lock_guard lock(g_token_handler_mutex);
  g_token_changed_handler = std::move(handler);
}

void IPushDeviceRegistrar::NotifyTokenChanged(const std::string& token) {
  TokenChangedFn handler;
  {
    std::lock_guard lock(g_token_handler_mutex);
    handler = g_token_changed_handler;
  }
  if (handler && !token.empty()) {
    handler(token);
  }
}

} // namespace pbr
