#include "base/ai/mcp/McpClient.h"
#include "base/net/ServiceClientFactory.h"

#include <gtest/gtest.h>

TEST(ServiceClientFactoryTest, BuildsHttpMockAndMcpClients) {
  pbr::AppConfig config;
  config.relay.base_url = "https://relay.example";

  auto clients = pbr::CreateServiceClients(config, nullptr);
  ASSERT_TRUE(static_cast<bool>(clients.relay));
  ASSERT_TRUE(static_cast<bool>(clients.directory));
  ASSERT_TRUE(static_cast<bool>(clients.registration));

  pbr::RelayEnvelope envelope;
  envelope.thread_id = "thread-1";
  envelope.message_id = "msg-1";
  envelope.sender_relay_id = "relay:self";
  envelope.body.text = "hello";
  const auto http_send = clients.relay->Send(envelope);
  EXPECT_FALSE(static_cast<bool>(http_send));

  pbr::AppConfig mock_config;
  auto mock_clients = pbr::CreateServiceClients(mock_config, nullptr);
  const auto mock_send = mock_clients.relay->Send(envelope);
  EXPECT_TRUE(static_cast<bool>(mock_send));

  auto& promoted = pbr::McpClient::MockInstance();
  auto mcp_clients = pbr::CreateServiceClients(mock_config, &promoted);
  auto mcp_hits = mcp_clients.directory->SearchPeople("alice");
  ASSERT_TRUE(static_cast<bool>(mcp_hits));
  ASSERT_FALSE((*mcp_hits).empty());
  EXPECT_EQ((*mcp_hits)[0].nickname, "alice");

  const auto mcp_send = mcp_clients.relay->Send(envelope);
  EXPECT_TRUE(static_cast<bool>(mcp_send));
}
