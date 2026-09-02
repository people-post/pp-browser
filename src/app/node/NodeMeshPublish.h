#pragma once

#include "foundation/data/Config.h"
#include "base/net/ServiceClients.h"
#include "base/people/IdentityStore.h"

#include "common/Error.h"

namespace pbr {

/**
 * Register/renew this pp-node as entity_kind=mesh_node (N027).
 * Uses registration.base_url + advertise_multiaddrs + local capabilities.
 * No-op (Ok) when mesh_publish is false or registration URL empty.
 */
Roe<bool> PublishOrRenewMeshNodeListing(const AppConfig& config, IdentityStore& identity,
                                        const std::string& nickname);

} // namespace pbr
