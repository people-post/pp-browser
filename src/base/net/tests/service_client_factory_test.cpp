#include "base/messaging/MessagingJson.h"
#include "base/messaging/RelayWirePayload.h"
#include "base/net/ServiceClientFactory.h"
#include "base/net/ServiceClientsImpl.h"

#include <gtest/gtest.h>

TEST(ServiceClientFactoryTest, BuildsHttpClientsWhenConfigured) {
  pbr::AppConfig config;
  config.relay.base_url = "https://relay.example";
  config.directory.base_url = "https://directory.example";
  config.registration.base_url = "https://registration.example";

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
}

TEST(ServiceClientFactoryTest, LeavesClientsUnsetWhenBaseUrlEmpty) {
  pbr::AppConfig empty_config;
  auto empty_clients = pbr::CreateServiceClients(empty_config);
  EXPECT_FALSE(static_cast<bool>(empty_clients.relay));
  EXPECT_FALSE(static_cast<bool>(empty_clients.directory));
  EXPECT_FALSE(static_cast<bool>(empty_clients.registration));
}

TEST(ServiceClientFactoryTest, MockClientsRemainAvailableForTests) {
  pbr::MockRelayClient mock_relay;
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
  EXPECT_TRUE(static_cast<bool>(mock_relay.Send(envelope)));
}
