#include "base/net/ServiceClientFactory.h"

#include "base/net/HttpBlobClient.h"
#include "base/net/ServiceClientsImpl.h"
#include "common/Logger.h"
#include "common/PbrCompat.h"

namespace pbr {

ServiceClients CreateServiceClients(const AppConfig& config) {
  ServiceClients clients;

  if (!config.relay.base_url.empty()) {
    clients.relay = std::make_unique<HttpRelayClient>(config.relay.base_url);
    clients.client_compat = std::make_unique<HttpClientCompatClient>(config.relay.base_url);
  } else {
    logging::getLogger("ServiceClientFactory").warning
        << "relay.base_url is empty; relay client not created";
  }

  {
    const std::vector<ServiceEndpointConfig> providers = EffectiveDirectoryProviders(config.directory);
    std::vector<std::unique_ptr<IDirectoryClient>> backends;
    backends.reserve(providers.size());
    for (const ServiceEndpointConfig& provider : providers) {
      const std::string transport = provider.transport.empty() ? "http" : provider.transport;
      if (transport != "http") {
        logging::getLogger("ServiceClientFactory").warning
            << "directory provider transport '" << transport
            << "' requires MeshHost (N029 nd4 Amp twin); skipping " << provider.base_url
            << " — MessagingHub wires Amp via AmpDirectoryService";
        continue;
      }
      backends.push_back(std::make_unique<HttpDirectoryClient>(provider.base_url));
    }
    if (backends.size() == 1) {
      clients.directory = std::move(backends.front());
    } else if (backends.size() > 1) {
      clients.directory = std::make_unique<FailoverDirectoryClient>(std::move(backends));
    } else {
      logging::getLogger("ServiceClientFactory").warning
          << "directory providers empty; directory client not created";
    }
  }

  if (!config.registration.base_url.empty()) {
    clients.registration = std::make_unique<HttpRegistrationClient>(config.registration.base_url);
    clients.blob = std::make_unique<HttpBlobClient>(config.registration.base_url);
  } else {
    logging::getLogger("ServiceClientFactory").warning
        << "registration.base_url is empty; registration client not created";
  }

  return clients;
}

} // namespace pbr
