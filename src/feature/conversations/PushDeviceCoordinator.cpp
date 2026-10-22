#include "feature/conversations/PushDeviceCoordinator.h"

#include "domain/net/IPushDeviceClient.h"
#include "foundation/platform/IPushDeviceRegistrar.h"
#include "feature/conversations/ConversationsHub.h"
#include "common/PbrCompat.h"

namespace pbr {

namespace {

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
  if (!hub.IsMessagingReady()) {
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
  auto registered = client->RegisterDevice(reg);
  // A CallKit iPhone's calls come by VoIP push, to their own token (P013).
  if (const std::string call_token = IPushDeviceRegistrar::Instance().CallPushToken(); !call_token.empty()) {
    PushDeviceRegistration call_reg = reg;
    call_reg.platform = IPushDeviceRegistrar::kCallPushPlatform;
    call_reg.device_id = reg.device_id + IPushDeviceRegistrar::kCallPushDeviceSuffix;
    call_reg.push_token = call_token;
    if (auto call_registered = client->RegisterDevice(call_reg); !call_registered && registered) {
      return call_registered;
    }
  }
  return registered;
}

} // namespace pbr
