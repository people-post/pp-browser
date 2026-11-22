#include "feature/conversations/MessagingCompatPorts.h"

#include "feature/conversations/ConversationsHub.h"

namespace pbr {

MessagingCompatPorts MakeMessagingCompatPorts(ConversationsHub& hub) {
  MessagingCompatPorts ports;
  ports.messaging_initialized = [&hub]() { return hub.IsInitialized(); };
  ports.client_compat = [&hub]() { return hub.ClientCompat(); };
  ports.profile_data_dir = [&hub]() { return hub.ProfileDataDir(); };
  ports.watch_upgrade_required = [&hub](std::function<void()> upgrade_required) {
    hub.SetOnOrgRelayRequiresUpgrade(std::move(upgrade_required));
  };
  ports.relay_certificate = [&hub](const std::string& certificate_b64) {
    hub.OfferOrgRelayCertificate(certificate_b64);
  };
  return ports;
}

} // namespace pbr
