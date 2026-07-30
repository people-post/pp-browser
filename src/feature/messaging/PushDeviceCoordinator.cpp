#include "feature/messaging/PushDeviceCoordinator.h"

#include "base/net/IPushDeviceClient.h"
#include "base/platform/IPushDeviceRegistrar.h"
#include "feature/messaging/MessagingHub.h"

namespace pbr {

namespace {

PushDeviceRegistration MakeRegistration(MessagingHub& hub, const std::string& token) {
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

Roe<void> PushDeviceCoordinator::UnregisterCurrent(MessagingHub& hub) {
  if (!hub.IsMessagingReady()) {
    return {};
  }
  IPushDeviceClient* client = hub.PushDevices();
  if (client == nullptr || !IPushDeviceRegistrar::Instance().IsSupported()) {
    return {};
  }
  auto token = IPushDeviceRegistrar::Instance().GetPushToken();
  PushDeviceRegistration reg = MakeRegistration(hub, token ? *token : std::string{});
  if (reg.relay_user_id.empty() || reg.device_id.empty()) {
    return {};
  }
  return client->UnregisterDevice(reg);
}

Roe<void> PushDeviceCoordinator::SyncWithPreference(MessagingHub& hub, const bool show_notifications) {
  if (!show_notifications) {
    return UnregisterCurrent(hub);
  }
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
  return client->RegisterDevice(reg);
}

} // namespace pbr
