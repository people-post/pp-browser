#include "base/messaging/MessagingJson.h"
#include "base/messaging/RelayWirePayload.h"
#include "base/net/ServiceClientFactory.h"

#include <gtest/gtest.h>

TEST(ServiceClientFactoryTest, BuildsHttpAndMockClients) {
  pbr::AppConfig config;
  config.relay.base_url = "https://relay.example";

  auto clients = pbr::CreateServiceClients(config);
  ASSERT_TRUE(static_cast<bool>(clients.relay));
  ASSERT_TRUE(static_cast<bool>(clients.directory));
  ASSERT_TRUE(static_cast<bool>(clients.registration));

  auto payload_b64 = pbr::RelayWirePayload::EncodePlaintextText("hello");
  ASSERT_TRUE(static_cast<bool>(payload_b64));

  pbr::RelayEnvelope envelope;
  envelope.envelope_version = pbr::kRelayEnvelopeVersion;
  envelope.message_id = "msg-1";
  envelope.sender_relay_id = "relay:self";
  envelope.sender_contact_id = "relay:self";
  envelope.route.kind = "direct";
  envelope.route.channel = pbr::ThreadChannel::E2e;
  envelope.body.e2e.payload_b64 = *payload_b64;
  envelope.sender_seq = 1;
  envelope.order_key = 1;
  envelope.stream_key = "v1:e2e:1:relay:peer:relay:self";
  envelope.recipient_contact_id = "relay:peer";
  envelope.timestamp = 1;
  const auto http_send = clients.relay->Send(envelope);
  EXPECT_FALSE(static_cast<bool>(http_send));

  pbr::AppConfig mock_config;
  auto mock_clients = pbr::CreateServiceClients(mock_config);
  const auto mock_send = mock_clients.relay->Send(envelope);
  EXPECT_TRUE(static_cast<bool>(mock_send));
}
