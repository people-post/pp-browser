#pragma once

#include "base/data/Config.h"
#include "base/net/BlobClient.h"
#include "base/net/ClientCompat.h"
#include "base/net/ServiceClients.h"

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
