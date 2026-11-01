#include "lib/net/OrgBackendClientFactory.h"

#include "lib/net/HttpBlobClient.h"
#include "lib/net/OrgBackendClientsImpl.h"
#include "common/Logger.h"
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

OrgBackendClients CreateOrgBackendClients(const AppConfig& config) {
  OrgBackendClients clients;

  // The relay's message client is messaging's (lib/messaging/HttpRelayClient): the conversations hub builds it.
  if (!config.relay.base_url.empty()) {
    clients.client_compat = std::make_unique<HttpClientCompatClient>(config.relay.base_url);
  } else {
    logging::getLogger("OrgBackendClientFactory").warning
        << "relay.base_url is empty; client compat client not created";
  }

  {
    const std::vector<ServiceEndpointConfig> providers = EffectiveDirectoryProviders(config.directory);
    std::vector<std::unique_ptr<mesh::IDirectoryClient>> backends;
    backends.reserve(providers.size());
    for (const ServiceEndpointConfig& provider : providers) {
      const std::string transport = provider.transport.empty() ? "http" : provider.transport;
      if (transport != "http") {
        logging::getLogger("OrgBackendClientFactory").warning
            << "directory provider transport '" << transport
            << "' requires MeshHost (N029 nd4 Amp twin); skipping " << provider.base_url
            << " — ConversationsHub wires Amp via AmpDirectoryProtocol";
        continue;
      }
      backends.push_back(std::make_unique<HttpDirectoryClient>(provider.base_url));
    }
    if (backends.size() == 1) {
      clients.directory = std::move(backends.front());
    } else if (backends.size() > 1) {
      clients.directory = std::make_unique<FailoverDirectoryClient>(std::move(backends));
    } else {
      logging::getLogger("OrgBackendClientFactory").warning
          << "directory providers empty; directory client not created";
    }
  }

  if (!config.registration.base_url.empty()) {
    clients.registration = std::make_unique<HttpRegistrationClient>(config.registration.base_url);
    clients.blob = std::make_unique<HttpBlobClient>(config.registration.base_url);
  } else {
    logging::getLogger("OrgBackendClientFactory").warning
        << "registration.base_url is empty; registration client not created";
  }

  return clients;
}

} // namespace pbr
