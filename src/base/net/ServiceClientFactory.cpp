#include "base/net/ServiceClientFactory.h"

#include "base/net/McpDirectoryClient.h"
#include "base/net/McpInfraBridge.h"
#include "base/net/McpRegistrationClient.h"
#include "base/net/McpRelayClient.h"
#include "base/net/ServiceClientsImpl.h"

namespace pbr {

ServiceClients CreateServiceClients(const AppConfig& config, McpClient* promoted_mcp) {
  ServiceClients clients;

  if (!config.relay.base_url.empty()) {
    clients.relay = std::make_unique<HttpRelayClient>(config.relay.base_url);
  } else if (PromotedMcpInfraAvailable(promoted_mcp)) {
    clients.relay = std::make_unique<McpRelayClient>(promoted_mcp);
  } else {
    clients.relay = std::make_unique<MockRelayClient>();
  }

  if (!config.directory.base_url.empty()) {
    clients.directory = std::make_unique<HttpDirectoryClient>(config.directory.base_url);
  } else if (PromotedMcpInfraAvailable(promoted_mcp)) {
    clients.directory = std::make_unique<McpDirectoryClient>(promoted_mcp);
  } else {
    clients.directory = std::make_unique<MockDirectoryClient>();
  }

  if (!config.registration.base_url.empty()) {
    clients.registration = std::make_unique<HttpRegistrationClient>(config.registration.base_url);
  } else if (PromotedMcpInfraAvailable(promoted_mcp)) {
    clients.registration = std::make_unique<McpRegistrationClient>(promoted_mcp);
  } else {
    clients.registration = std::make_unique<MockRegistrationClient>();
  }

  return clients;
}

} // namespace pbr
