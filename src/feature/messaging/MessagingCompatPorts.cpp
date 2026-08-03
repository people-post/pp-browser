#include "feature/messaging/MessagingCompatPorts.h"

#include "feature/messaging/MessagingHub.h"

namespace pbr {

MessagingCompatPorts MakeMessagingCompatPorts(MessagingHub& hub) {
  MessagingCompatPorts ports;
  ports.messaging_initialized = [&hub]() { return hub.IsInitialized(); };
  ports.client_compat = [&hub]() { return hub.ClientCompat(); };
  ports.profile_data_dir = [&hub]() { return hub.ProfileDataDir(); };
  return ports;
}

} // namespace pbr
