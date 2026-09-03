#pragma once

#include "domain/net/ServiceClients.h"

#include <functional>
#include <string>

namespace pbr {

class IClientCompatClient;

/**
 * Client-compat fetch ports. Application fills from ConversationsHub.
 * Clear via BindCompatPorts({}).
 */
struct MessagingCompatPorts {
  std::function<bool()> messaging_initialized;
  std::function<IClientCompatClient*()> client_compat;
  std::function<std::string()> profile_data_dir;
};

class ConversationsHub;

MessagingCompatPorts MakeMessagingCompatPorts(ConversationsHub& hub);

} // namespace pbr
