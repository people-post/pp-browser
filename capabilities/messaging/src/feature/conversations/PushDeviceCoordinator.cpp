#include "feature/conversations/PushDeviceCoordinator.h"

#include "lib/net/IPushDeviceClient.h"
#include "common/IPushDeviceRegistrar.h"
#include "common/Logger.h"
#include "common/runtime/AppRuntime.h"
#include "foundation/error/AppError.h"
#include "feature/conversations/ConversationsHub.h"
#include "common/PbrCompat.h"

namespace pbr {

namespace {

logging::Logger& PushLog() {
  static logging::Logger log = logging::getLogger("PushDeviceCoordinator");
  return log;
}

PushDeviceRegistration MakeRegistration(ConversationsHub& hub, const std::string& token) {
  PushDeviceRegistration reg;
  reg.platform = IPushDeviceRegistrar::Instance().PlatformName();
  reg.device_id = IPushDeviceRegistrar::Instance().DeviceId();
  reg.push_token = token;
  auto identity = hub.Identity().Get();
  if (identity) {
    reg.relay_user_id = identity->relay_user_id;
  }
  return reg;
}

} // namespace

Roe<void> PushDeviceCoordinator::Register(ConversationsHub& hub) {
  // Registered whatever "Show notifications" says (P005 / P010): in the background a phone is
  // reached by push only, and calls ring with alerts off too; the setting hides message banners.
  if (!hub.IsMessagingReady() || hub.PushGate().Suspended()) {
    return {};
  }
  IPushDeviceClient* client = hub.PushDevices();
  if (client == nullptr || !IPushDeviceRegistrar::Instance().IsSupported()) {
    return {};
  }
  auto token = IPushDeviceRegistrar::Instance().GetPushToken();
  if (!token) {
    return token.error();
  }
  PushDeviceRegistration reg = MakeRegistration(hub, *token);
  if (reg.relay_user_id.empty()) {
    return Error("Relay user not registered");
  }
  return client->RegisterDevice(reg);
}

Roe<void> PushDeviceCoordinator::Unregister(ConversationsHub& hub) {
  IPushDeviceClient* client = hub.PushDevices();
  if (client == nullptr || !IPushDeviceRegistrar::Instance().IsSupported()) {
    return {};
  }
  if (!hub.IsMessagingReady()) {
    return Error("Messaging is not ready");
  }
  auto token = IPushDeviceRegistrar::Instance().GetPushToken();
  if (!token) {
    return token.error();
  }
  PushDeviceRegistration reg = MakeRegistration(hub, *token);
  if (reg.relay_user_id.empty()) {
    return {};
  }
  return client->UnregisterDevice(reg);
}

void PushDeviceCoordinator::OnBackendSwitchPending(ConversationsHub& hub, const bool pending) {
  switch (hub.PushGate().OnSwitchPending(pending)) {
    case PushBindingGate::Action::Unbind:
      AppRuntime::PostWorkerNormal([&hub]() {
        auto unbound = Unregister(hub);
        if (!unbound) {
          PushLog().warning << "Push unbind on the old backend failed: " << AppError::Log(unbound.error());
        }
        if (hub.PushGate().OnUnbound(static_cast<bool>(unbound))) {
          (void)Register(hub);
        }
      });
      break;
    case PushBindingGate::Action::Rebind:
      AppRuntime::PostWorkerNormal([&hub]() { (void)Register(hub); });
      break;
    case PushBindingGate::Action::None:
      break;
  }
}

} // namespace pbr
