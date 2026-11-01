#pragma once

#include "common/ServiceEndpoints.h"
#include "lib/net/BlobClient.h"
#include "lib/net/ClientCompat.h"
#include "lib/net/OrgBackendClients.h"
#include "common/MeshAlias.h"

#include <memory>

namespace pbr {

/** The backend's clients net owns (the relay's message client is messaging's: lib/messaging/HttpRelayClient). */
struct OrgBackendClients {
  std::unique_ptr<mesh::IDirectoryClient> directory;
  std::unique_ptr<IRegistrationClient> registration;
  std::unique_ptr<IBlobClient> blob;
  std::unique_ptr<IClientCompatClient> client_compat;
};

/** Clients for the backend at these endpoints (the app's config: relay, directory, registration). */
OrgBackendClients CreateOrgBackendClients(const ServiceEndpointConfig& relay, const DirectoryConfig& directory,
                                          const ServiceEndpointConfig& registration);

} // namespace pbr
