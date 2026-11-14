#pragma once

#include "lib/net/OrgBackendClients.h"

#include <functional>
#include <memory>
#include <string>

namespace pbr {

class IClientCompatClient;

/**
 * Client-compat fetch ports. Application fills from ConversationsHub.
 * Clear via BindCompatPorts({}).
 */
struct MessagingCompatPorts {
  std::function<bool()> messaging_initialized;
  /** Shared: a fetch on a worker keeps the client it started with when the org's relay changes. */
  std::function<std::shared_ptr<IClientCompatClient>()> client_compat;
  std::function<std::string()> profile_data_dir;
};

class ConversationsHub;

MessagingCompatPorts MakeMessagingCompatPorts(ConversationsHub& hub);

} // namespace pbr
