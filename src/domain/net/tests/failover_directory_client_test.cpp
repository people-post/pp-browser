#include "domain/net/OrgBackendClientsImpl.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

TEST(FailoverDirectoryClientTest, FallsBackWhenTheFirstBackendFails) {
  class FakeHttpDirectory : public IDirectoryClient {
  public:
    Roe<std::vector<DirectoryHit>> SearchPeople(const std::string&) override {
      return Error("unused");
    }
    Roe<DirectoryHit> LookupRelayUser(const std::string&) override { return Error("unused"); }
    Roe<DirectoryHit> LookupByAccount(const std::string&) override { return Error("unused"); }
    Roe<std::vector<MeshNodeHit>> ListMeshNodes() override {
      MeshNodeHit hit;
      hit.relay_user_id = "from-http";
      hit.entity_kind = "mesh_node";
      return std::vector<MeshNodeHit>{hit};
    }
  };

  class FailingAmpDirectory : public IDirectoryClient {
  public:
    Roe<std::vector<DirectoryHit>> SearchPeople(const std::string&) override {
      return Error("amp person unsupported");
    }
    Roe<DirectoryHit> LookupRelayUser(const std::string&) override {
      return Error("amp person unsupported");
    }
    Roe<DirectoryHit> LookupByAccount(const std::string&) override {
      return Error("amp person unsupported");
    }
    Roe<std::vector<MeshNodeHit>> ListMeshNodes() override { return Error("amp unavailable"); }
  };

  std::vector<std::unique_ptr<IDirectoryClient>> backends;
  backends.push_back(std::make_unique<FailingAmpDirectory>());
  backends.push_back(std::make_unique<FakeHttpDirectory>());
  FailoverDirectoryClient failover(std::move(backends));
  auto nodes = failover.ListMeshNodes();
  ASSERT_TRUE(static_cast<bool>(nodes)) << nodes.error().message;
  ASSERT_EQ(nodes->size(), 1u);
  EXPECT_EQ(nodes->front().relay_user_id, "from-http");
}

} // namespace
} // namespace pbr
