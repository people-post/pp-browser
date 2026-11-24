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
  /** Where client_compat.json is kept: the profile's `backends/{backend id}/` (profile-model PM004). */
  std::function<std::string()> backend_data_dir;
  /**
   * Subscribes `upgrade_required` (nullptr: unsubscribes): the org's relay answered a call `426
   * client_upgrade_required` (any thread; ConversationsHub::SetOnOrgRelayRequiresUpgrade).
   */
  std::function<void(std::function<void()> upgrade_required)> watch_upgrade_required;
  /**
   * The org relay's certificate a document carried (`relay_certificate_b64`, SIGNED_FORMATS § 3.4): offered to the
   * relay pins, which keep it when it verifies against the relay id the build names and is higher than the one held
   * (ConversationsHub::OfferOrgRelayCertificate).
   */
  std::function<void(const std::string& certificate_b64)> relay_certificate;
};

class ConversationsHub;

MessagingCompatPorts MakeMessagingCompatPorts(ConversationsHub& hub);

} // namespace pbr
