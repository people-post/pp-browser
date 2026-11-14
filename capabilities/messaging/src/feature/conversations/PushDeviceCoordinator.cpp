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
  if (identity && identity->registered) {
    reg.account_id = identity->account_id; // push tokens are registered per relay, by Account ID (MR001)
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
  if (reg.account_id.empty()) {
    return Error("Account not registered on the relay");
  }
  // Every relay of ours that holds our registration may hold mail, and wakes the phone for it (multi-relay R2). The
  // preferred relay's answer is the one reported; another's failure is logged and tried at the next sync.
  for (const auto& [origin, other] : hub.OtherRelayPushDevices()) {
    if (auto bound = other->RegisterDevice(reg); !bound) {
      PushLog().warning << "Push binding on another relay failed: " << AppError::Log(bound.error());
    }
  }
  return client->RegisterDevice(reg);
}

Roe<void> PushDeviceCoordinator::RegisterOn(ConversationsHub& hub, const std::string& origin) {
  if (!hub.IsMessagingReady() || hub.PushGate().Suspended() || !IPushDeviceRegistrar::Instance().IsSupported()) {
    return {};
  }
  for (const auto& [other_origin, other] : hub.OtherRelayPushDevices()) {
    if (other_origin != origin) {
      continue;
    }
    auto token = IPushDeviceRegistrar::Instance().GetPushToken();
    if (!token) {
      return token.error();
    }
    PushDeviceRegistration reg = MakeRegistration(hub, *token);
    if (reg.account_id.empty()) {
      return {};
    }
    return other->RegisterDevice(reg);
  }
  return {};
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
  if (reg.account_id.empty()) {
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
