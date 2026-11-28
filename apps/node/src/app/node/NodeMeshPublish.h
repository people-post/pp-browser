#pragma once

#include "app/node/NodeConfig.h"
#include "lib/net/OrgBackendClients.h"
#include "lib/net/RelayAudience.h"
#include "lib/people/IdentityStore.h"

#include "common/Error.h"

#include <memory>

namespace pbr {

/**
 * Register/renew this pp-node as entity_kind=mesh_node (N027).
 * Uses registration.base_url + advertise_multiaddrs + local capabilities, signed for the relay `audience` names (its
 * expected PeerId, multi-relay MR007 / MR008: nothing is signed while it does not match its address or none is known).
 * No-op (Ok) when mesh_publish is false or registration URL empty.
 */
Roe<bool> PublishOrRenewMeshNodeListing(const NodeConfig& config, IdentityStore& identity, const std::string& nickname,
                                        std::shared_ptr<IRelayAudience> audience);

} // namespace pbr
