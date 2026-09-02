#include "base/messaging/MessagingJson.h"
#include "base/messaging/RelayWirePayload.h"
#include "base/net/ServiceClientFactory.h"
#include "base/net/ServiceClientsImpl.h"

#include <gtest/gtest.h>

#include <memory>

TEST(ServiceClientFactoryTest, BuildsHttpClientsWhenConfigured) {
  pbr::AppConfig config;
  config.relay.base_url = "https://relay.example";
  config.directory.base_url = "https://directory.example";
  config.registration.base_url = "https://registration.example";

  auto clients = pbr::CreateServiceClients(config);
  ASSERT_TRUE(static_cast<bool>(clients.relay));
  ASSERT_TRUE(static_cast<bool>(clients.directory));
  ASSERT_TRUE(static_cast<bool>(clients.registration));
  ASSERT_TRUE(static_cast<bool>(clients.blob));
  ASSERT_TRUE(static_cast<bool>(clients.client_compat));

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
  EXPECT_FALSE(static_cast<bool>(empty_clients.blob));
  EXPECT_FALSE(static_cast<bool>(empty_clients.client_compat));
}

TEST(ServiceClientFactoryTest, BuildsFailoverDirectoryFromProviders) {
  pbr::AppConfig config;
  config.directory.providers = {{"https://dir-a.example", "http"}, {"https://dir-b.example", "http"}};
  auto clients = pbr::CreateServiceClients(config);
  ASSERT_TRUE(static_cast<bool>(clients.directory));
  // Failover client is opaque; a call fails against unreachable hosts but must route through the wrapper.
  const auto nodes = clients.directory->ListMeshNodes();
  EXPECT_FALSE(static_cast<bool>(nodes));
}

TEST(ServiceClientFactoryTest, SkipsNonHttpDirectoryProviders) {
  pbr::AppConfig config;
  config.directory.providers = {{"https://dir-a.example", "amp"}, {"https://dir-b.example", "http"}};
  auto clients = pbr::CreateServiceClients(config);
  ASSERT_TRUE(static_cast<bool>(clients.directory));
}

namespace {

class ScriptedDirectoryClient : public pbr::IDirectoryClient {
public:
  explicit ScriptedDirectoryClient(bool fail) : fail_(fail) {}

  pbr::Roe<std::vector<pbr::DirectoryHit>> SearchPeople(const std::string& /*query*/) override {
    if (fail_) {
      return pbr::Error("scripted search fail");
    }
    return std::vector<pbr::DirectoryHit>{};
  }
  pbr::Roe<pbr::DirectoryHit> LookupRelayUser(const std::string& /*relay_user_id*/) override {
    return pbr::Error("unused");
  }
  pbr::Roe<pbr::DirectoryHit> LookupByAccount(const std::string& /*account_id*/) override {
    return pbr::Error("unused");
  }
  pbr::Roe<std::vector<pbr::MeshNodeHit>> ListMeshNodes() override {
    ++list_calls;
    if (fail_) {
      return pbr::Error("scripted list fail");
    }
    pbr::MeshNodeHit hit;
    hit.relay_user_id = "relay:ok";
    return std::vector<pbr::MeshNodeHit>{hit};
  }

  int list_calls = 0;

private:
  bool fail_ = false;
};

} // namespace

TEST(ServiceClientFactoryTest, FailoverDirectoryClientUsesSecondBackend) {
  std::vector<std::unique_ptr<pbr::IDirectoryClient>> backends;
  auto first = std::make_unique<ScriptedDirectoryClient>(true);
  auto second = std::make_unique<ScriptedDirectoryClient>(false);
  auto* second_ptr = second.get();
  backends.push_back(std::move(first));
  backends.push_back(std::move(second));
  pbr::FailoverDirectoryClient failover(std::move(backends));
  auto nodes = failover.ListMeshNodes();
  ASSERT_TRUE(static_cast<bool>(nodes));
  ASSERT_EQ(nodes->size(), 1u);
  EXPECT_EQ(nodes->front().relay_user_id, "relay:ok");
  EXPECT_EQ(second_ptr->list_calls, 1);
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
