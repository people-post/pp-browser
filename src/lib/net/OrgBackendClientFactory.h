#pragma once

#include "foundation/data/Config.h"
#include "lib/net/BlobClient.h"
#include "lib/net/ClientCompat.h"
#include "lib/net/OrgBackendClients.h"
#include "common/MeshAlias.h"

#include <memory>

namespace pbr {

struct OrgBackendClients {
  std::unique_ptr<IRelayClient> relay;
  std::unique_ptr<mesh::IDirectoryClient> directory;
  std::unique_ptr<IRegistrationClient> registration;
  std::unique_ptr<IBlobClient> blob;
  std::unique_ptr<IClientCompatClient> client_compat;
};

OrgBackendClients CreateOrgBackendClients(const AppConfig& config);

} // namespace pbr
