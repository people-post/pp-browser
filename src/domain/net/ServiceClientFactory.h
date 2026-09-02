#pragma once

#include "foundation/data/Config.h"
#include "domain/net/BlobClient.h"
#include "domain/net/ClientCompat.h"
#include "domain/net/ServiceClients.h"

#include <memory>

namespace pbr {

struct ServiceClients {
  std::unique_ptr<IRelayClient> relay;
  std::unique_ptr<IDirectoryClient> directory;
  std::unique_ptr<IRegistrationClient> registration;
  std::unique_ptr<IBlobClient> blob;
  std::unique_ptr<IClientCompatClient> client_compat;
};

ServiceClients CreateServiceClients(const AppConfig& config);

} // namespace pbr
