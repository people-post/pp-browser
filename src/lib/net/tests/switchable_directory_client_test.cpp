#include "lib/net/SwitchableDirectoryClient.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {
namespace {

class NamedDirectory : public mesh::IDirectoryClient {
public:
  explicit NamedDirectory(std::string name) : name_(std::move(name)) {}
  Roe<std::vector<mesh::DirectoryHit>> SearchPeople(const std::string&) override {
    return std::vector<mesh::DirectoryHit>{Hit()};
  }
  Roe<mesh::DirectoryHit> LookupRelayUser(const std::string&) override { return Hit(); }
  Roe<mesh::DirectoryHit> LookupByAccount(const std::string&) override { return Hit(); }

private:
  mesh::DirectoryHit Hit() const {
    mesh::DirectoryHit hit;
    hit.hit_id = name_;
    return hit;
  }
  std::string name_;
};

TEST(SwitchableDirectoryClientTest, WithNoBackendEveryLookupSaysNotConfigured) {
  SwitchableDirectoryClient directory;
  EXPECT_FALSE(directory.Configured());
  EXPECT_FALSE(directory.SearchPeople("alice"));
  EXPECT_FALSE(directory.LookupRelayUser("relay:a"));
  EXPECT_FALSE(directory.LookupByAccount("account:a"));
  EXPECT_FALSE(directory.ListMeshNodes());
}

TEST(SwitchableDirectoryClientTest, AHolderFollowsTheConfiguredBackendAcrossChanges) {
  SwitchableDirectoryClient directory;
  mesh::IDirectoryClient& held = directory;  // what a resolver keeps

  directory.SetBackend(std::make_shared<NamedDirectory>("first"));
  EXPECT_TRUE(directory.Configured());
  EXPECT_EQ(held.LookupByAccount("account:a")->hit_id, "first");

  directory.SetBackend(std::make_shared<NamedDirectory>("second"));
  EXPECT_EQ(held.LookupByAccount("account:a")->hit_id, "second");

  directory.SetBackend(nullptr);
  EXPECT_FALSE(held.LookupByAccount("account:a"));
}

} // namespace
} // namespace pbr
