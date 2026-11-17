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
  /**
   * Subscribes `upgrade_required` (nullptr: unsubscribes): the org's relay answered a call `426
   * client_upgrade_required` (any thread; ConversationsHub::SetOnOrgRelayRequiresUpgrade).
   */
  std::function<void(std::function<void()> upgrade_required)> watch_upgrade_required;
};

class ConversationsHub;

MessagingCompatPorts MakeMessagingCompatPorts(ConversationsHub& hub);

} // namespace pbr
