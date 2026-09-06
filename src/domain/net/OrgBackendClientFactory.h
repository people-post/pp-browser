#pragma once

#include "foundation/data/Config.h"
#include "domain/net/BlobClient.h"
#include "domain/net/ClientCompat.h"
#include "domain/net/OrgBackendClients.h"

#include <memory>

namespace pbr {

struct OrgBackendClients {
  std::unique_ptr<IRelayClient> relay;
  std::unique_ptr<IDirectoryClient> directory;
  std::unique_ptr<IRegistrationClient> registration;
  std::unique_ptr<IBlobClient> blob;
  std::unique_ptr<IClientCompatClient> client_compat;
};

OrgBackendClients CreateOrgBackendClients(const AppConfig& config);

} // namespace pbr
