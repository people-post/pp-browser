#include "base/ai/mcp/McpClient.h"
#include "base/net/ServiceClientFactory.h"

#include <cassert>
#include <iostream>

int main() {
  pbr::AppConfig config;
  config.relay.base_url = "https://relay.example";

  auto clients = pbr::CreateServiceClients(config, nullptr);
  assert(clients.relay);
  assert(clients.directory);
  assert(clients.registration);

  pbr::RelayEnvelope envelope;
  envelope.thread_id = "thread-1";
  envelope.message_id = "msg-1";
  envelope.sender_relay_id = "relay:self";
  envelope.body.text = "hello";
  const auto http_send = clients.relay->Send(envelope);
  assert(!http_send);

  pbr::AppConfig mock_config;
  auto mock_clients = pbr::CreateServiceClients(mock_config, nullptr);
  const auto mock_send = mock_clients.relay->Send(envelope);
  assert(mock_send);

  auto& promoted = pbr::McpClient::MockInstance();
  auto mcp_clients = pbr::CreateServiceClients(mock_config, &promoted);
  const auto mcp_hits = mcp_clients.directory->SearchPeople("alice");
  assert(mcp_hits);
  assert(!mcp_hits->empty());
  assert((*mcp_hits)[0].nickname == "alice");

  const auto mcp_send = mcp_clients.relay->Send(envelope);
  assert(mcp_send);

  std::cout << "service_client_factory_test ok\n";
  return 0;
}
