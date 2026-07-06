#include "base/net/ServiceClientFactory.h"

#include "base/net/ServiceClientsImpl.h"

namespace pbr {

ServiceClients CreateServiceClients(const AppConfig& config) {
  ServiceClients clients;

  if (!config.relay.base_url.empty()) {
    clients.relay = std::make_unique<HttpRelayClient>(config.relay.base_url);
  } else {
    clients.relay = std::make_unique<MockRelayClient>();
  }

  if (!config.directory.base_url.empty()) {
    clients.directory = std::make_unique<HttpDirectoryClient>(config.directory.base_url);
  } else {
    clients.directory = std::make_unique<MockDirectoryClient>();
  }

  if (!config.registration.base_url.empty()) {
    clients.registration = std::make_unique<HttpRegistrationClient>(config.registration.base_url);
  } else {
    clients.registration = std::make_unique<MockRegistrationClient>();
  }

  return clients;
}

} // namespace pbr
