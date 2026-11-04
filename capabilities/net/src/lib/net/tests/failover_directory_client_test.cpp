#include "lib/net/OrgBackendClientsImpl.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {
namespace {

TEST(FailoverDirectoryClientTest, FallsBackWhenTheFirstBackendFails) {
  class FakeHttpDirectory : public mesh::IDirectoryClient {
  public:
    Roe<std::vector<mesh::DirectoryHit>> SearchPeople(const std::string&) override {
      return Error("unused");
    }
    Roe<mesh::DirectoryHit> LookupRelayUser(const std::string&) override { return Error("unused"); }
    Roe<mesh::DirectoryHit> LookupByAccount(const std::string&) override { return Error("unused"); }
    Roe<std::vector<mesh::MeshNodeHit>> ListMeshNodes() override {
      mesh::MeshNodeHit hit;
      hit.relay_user_id = "from-http";
      hit.entity_kind = "mesh_node";
      return std::vector<mesh::MeshNodeHit>{hit};
    }
  };

  class FailingAmpDirectory : public mesh::IDirectoryClient {
  public:
    Roe<std::vector<mesh::DirectoryHit>> SearchPeople(const std::string&) override {
      return Error("amp person unsupported");
    }
    Roe<mesh::DirectoryHit> LookupRelayUser(const std::string&) override {
      return Error("amp person unsupported");
    }
    Roe<mesh::DirectoryHit> LookupByAccount(const std::string&) override {
      return Error("amp person unsupported");
    }
    Roe<std::vector<mesh::MeshNodeHit>> ListMeshNodes() override { return Error("amp unavailable"); }
  };

  std::vector<std::unique_ptr<mesh::IDirectoryClient>> backends;
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
