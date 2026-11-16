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

/** The VoIP binding beside `reg`: its own platform, device id and token. */
PushDeviceRegistration VoipBinding(const PushDeviceRegistration& reg, const std::string& voip_token) {
  PushDeviceRegistration call_reg = reg;
  call_reg.platform = IPushDeviceRegistrar::kVoipPushPlatform;
  call_reg.device_id = reg.device_id + IPushDeviceRegistrar::kVoipPushDeviceSuffix;
  call_reg.push_token = voip_token;
  return call_reg;
}

} // namespace

Roe<void> PushDeviceCoordinator::RegisterWith(IPushDeviceClient& client, const PushDeviceRegistration& reg,
                                              const std::string& voip_token, const bool voip_wanted) {
  auto result = client.RegisterDevice(reg);
  if (voip_token.empty()) {
    return result;
  }
  // A custom PIN has no background key: its calls ring by alert, so the VoIP binding goes.
  auto voip_result = voip_wanted ? client.RegisterDevice(VoipBinding(reg, voip_token))
                                 : client.UnregisterDevice(VoipBinding(reg, voip_token));
  return result ? voip_result : result;
}

Roe<void> PushDeviceCoordinator::UnregisterWith(IPushDeviceClient& client, const PushDeviceRegistration& reg,
                                                const std::string& voip_token) {
  auto result = client.UnregisterDevice(reg);
  if (voip_token.empty()) {
    return result;
  }
  auto voip_result = client.UnregisterDevice(VoipBinding(reg, voip_token));
  return result ? voip_result : result;
}

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
  const std::string voip_token = IPushDeviceRegistrar::Instance().VoipPushToken();
  const bool voip_wanted = IPushDeviceRegistrar::VoipPushWanted();
  for (const auto& [origin, other] : hub.OtherRelayPushDevices()) {
    if (auto bound = RegisterWith(*other, reg, voip_token, voip_wanted); !bound) {
      PushLog().warning << "Push binding on another relay failed: " << AppError::Log(bound.error());
    }
  }
  return RegisterWith(*client, reg, voip_token, voip_wanted);
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
    return RegisterWith(*other, reg, IPushDeviceRegistrar::Instance().VoipPushToken(),
                        IPushDeviceRegistrar::VoipPushWanted());
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
  return UnregisterWith(*client, reg, IPushDeviceRegistrar::Instance().VoipPushToken());
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
