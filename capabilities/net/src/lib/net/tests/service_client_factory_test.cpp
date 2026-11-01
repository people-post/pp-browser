#include "lib/net/OrgBackendClientFactory.h"
#include "lib/net/testing/MockOrgBackendClients.h"

#include <gtest/gtest.h>

#include <memory>

namespace {

/** The endpoints a config names (relay, directory, registration). */
struct Endpoints {
  pbr::ServiceEndpointConfig relay;
  pbr::DirectoryConfig directory;
  pbr::ServiceEndpointConfig registration;
};

} // namespace

TEST(OrgBackendClientFactoryTest, BuildsHttpClientsWhenConfigured) {
  Endpoints config;
  config.relay.base_url = "https://relay.example";
  config.directory.base_url = "https://directory.example";
  config.registration.base_url = "https://registration.example";

  auto clients = pbr::CreateOrgBackendClients(config.relay, config.directory, config.registration);
  ASSERT_TRUE(static_cast<bool>(clients.directory));
  ASSERT_TRUE(static_cast<bool>(clients.registration));
  ASSERT_TRUE(static_cast<bool>(clients.blob));
  ASSERT_TRUE(static_cast<bool>(clients.client_compat));
}

TEST(OrgBackendClientFactoryTest, LeavesClientsUnsetWhenBaseUrlEmpty) {
  Endpoints empty_config;
  auto empty_clients = pbr::CreateOrgBackendClients(empty_config.relay, empty_config.directory, empty_config.registration);
  EXPECT_FALSE(static_cast<bool>(empty_clients.directory));
  EXPECT_FALSE(static_cast<bool>(empty_clients.registration));
  EXPECT_FALSE(static_cast<bool>(empty_clients.blob));
  EXPECT_FALSE(static_cast<bool>(empty_clients.client_compat));
}

TEST(OrgBackendClientFactoryTest, BuildsFailoverDirectoryFromProviders) {
  Endpoints config;
  config.directory.providers = {{"https://dir-a.example", "http"}, {"https://dir-b.example", "http"}};
  auto clients = pbr::CreateOrgBackendClients(config.relay, config.directory, config.registration);
  ASSERT_TRUE(static_cast<bool>(clients.directory));
  // Failover client is opaque; a call fails against unreachable hosts but must route through the wrapper.
  const auto nodes = clients.directory->ListMeshNodes();
  EXPECT_FALSE(static_cast<bool>(nodes));
}

TEST(OrgBackendClientFactoryTest, SkipsNonHttpDirectoryProviders) {
  Endpoints config;
  config.directory.providers = {{"https://dir-a.example", "amp"}, {"https://dir-b.example", "http"}};
  auto clients = pbr::CreateOrgBackendClients(config.relay, config.directory, config.registration);
  ASSERT_TRUE(static_cast<bool>(clients.directory));
}

namespace {


class ScriptedDirectoryClient : public pp::mesh::IDirectoryClient {
public:
  explicit ScriptedDirectoryClient(bool fail) : fail_(fail) {}

  pbr::Roe<std::vector<pp::mesh::DirectoryHit>> SearchPeople(const std::string& /*query*/) override {
    if (fail_) {
      return pbr::Error("scripted search fail");
    }
    return std::vector<pp::mesh::DirectoryHit>{};
  }
  pbr::Roe<pp::mesh::DirectoryHit> LookupRelayUser(const std::string& /*relay_user_id*/) override {
    return pbr::Error("unused");
  }
  pbr::Roe<pp::mesh::DirectoryHit> LookupByAccount(const std::string& /*account_id*/) override {
    return pbr::Error("unused");
  }
  pbr::Roe<std::vector<pp::mesh::MeshNodeHit>> ListMeshNodes() override {
    ++list_calls;
    if (fail_) {
      return pbr::Error("scripted list fail");
    }
    pp::mesh::MeshNodeHit hit;
    hit.relay_user_id = "relay:ok";
    return std::vector<pp::mesh::MeshNodeHit>{hit};
  }

  int list_calls = 0;

private:
  bool fail_ = false;
};

} // namespace

TEST(OrgBackendClientFactoryTest, FailoverDirectoryClientUsesSecondBackend) {
  std::vector<std::unique_ptr<pp::mesh::IDirectoryClient>> backends;
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
