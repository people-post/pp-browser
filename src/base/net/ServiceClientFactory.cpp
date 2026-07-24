#include "base/net/ServiceClientFactory.h"

#include "base/net/ServiceClientsImpl.h"
#include "common/Logger.h"

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

  if (!config.directory.base_url.empty()) {
    clients.directory = std::make_unique<HttpDirectoryClient>(config.directory.base_url);
  } else {
    logging::getLogger("ServiceClientFactory").warning
        << "directory.base_url is empty; directory client not created";
  }

  if (!config.registration.base_url.empty()) {
    clients.registration = std::make_unique<HttpRegistrationClient>(config.registration.base_url);
  } else {
    logging::getLogger("ServiceClientFactory").warning
        << "registration.base_url is empty; registration client not created";
  }

  return clients;
}

} // namespace pbr
