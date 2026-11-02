#include "feature/product/ProductMesh.h"

#include "common/Utilities.h"
#include "common/runtime/AppRuntime.h"
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/runtime/ProductOwners.h"
#include "mesh/l4/circuit/serve/CircuitRelayServer.h"
#include "mesh/l4/media_relay/serve/MediaRelayServer.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {
namespace {

ByteVector TestDek() {
  ByteVector dek(kDataEncryptionKeySize);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0x5a + i);
  }
  return dek;
}

// The app's mesh is built for the profile's role (capability-repos P1, messaging -> node): a client
// serves nothing inbound; a node serves what its capabilities say, and follows them when they change.
class ProductMeshTest : public ::testing::Test {
protected:
  void SetUp() override {
    AppRuntime::Initialize(ProductRuntimeConfig());
    dir_ = std::filesystem::temp_directory_path() / ("pp_browser_product_mesh_" + util::GenerateUuid());
    identity_ = std::make_unique<IdentityStore>(dir_.string(), "test");
    ASSERT_TRUE(identity_->SetDek(TestDek()));
    ASSERT_TRUE(identity_->LoadOrCreate());
  }
  void TearDown() override {
    mesh_.Stop();
    mesh_.Release();
    identity_.reset();
    AppRuntime::Shutdown();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  /** Starts the mesh on an ephemeral UDP port with no seeds: `node` with a circuit relay, no media relay. */
  mesh::MeshHost* Start(bool node) {
    HubMeshOwner::StartRequest request;
    request.mesh.mesh_enabled = true;
    request.mesh.amp_udp_port = 0;
    request.mesh.node_enabled = node;
    request.mesh.capabilities.circuit_relay = true;
    request.mesh.capabilities.media_relay = false;
    request.identity = identity_.get();
    request.address_disclosure = &disclosure_;
    auto started = mesh_.Start(std::move(request));
    EXPECT_TRUE(started) << (started ? "" : started.error().message);
    return started ? *started : nullptr;
  }

  std::filesystem::path dir_;
  std::unique_ptr<IdentityStore> identity_;
  mesh::AddressDisclosureGate disclosure_;
  ProductMesh mesh_;
};

TEST_F(ProductMeshTest, AClientServesNothingInbound) {
  mesh::MeshHost* mesh = Start(false);
  ASSERT_NE(mesh, nullptr);
  ASSERT_NE(mesh->Amp(), nullptr);
  if (auto* circuit = mesh->AmpCircuitServer()) {
    EXPECT_FALSE(circuit->ServeInbound());
  }
  MeshConfig config;
  config.node_enabled = false;
  config.capabilities.circuit_relay = true;
  mesh_.ApplyServing(config);
  if (auto* circuit = mesh->AmpCircuitServer()) {
    EXPECT_FALSE(circuit->ServeInbound()) << "a client serves nothing, whatever its capabilities say";
  }
}

TEST_F(ProductMeshTest, ANodeServesWhatItsCapabilitiesSay) {
  mesh::MeshHost* mesh = Start(true);
  ASSERT_NE(mesh, nullptr);
  ASSERT_NE(mesh->AmpCircuitServer(), nullptr) << "a node with a circuit relay hosts it";
  MeshConfig config;
  config.node_enabled = true;
  config.capabilities.circuit_relay = true;
  mesh_.ApplyServing(config);
  EXPECT_TRUE(mesh->AmpCircuitServer()->ServeInbound());

  config.capabilities.circuit_relay = false;
  mesh_.ApplyServing(config);
  EXPECT_FALSE(mesh->AmpCircuitServer()->ServeInbound()) << "the capability turned off: no new circuits";
}

TEST_F(ProductMeshTest, OneMeshAtATimeAndAStopFreesItForTheNext) {
  ASSERT_NE(Start(false), nullptr);
  HubMeshOwner::StartRequest again;
  again.identity = identity_.get();
  EXPECT_FALSE(mesh_.Start(std::move(again))) << "the running mesh is the hub's until it gives it back";

  mesh_.Stop();
  mesh_.Release();
  EXPECT_NE(Start(false), nullptr);
}

} // namespace
} // namespace pbr

namespace pbr {

// The last-good directory nodes of one backend must never seed another's: the file lives in the profile.
TEST(ProductMeshPathTest, DirectoryNodesFileIsPerProfile) {
  const std::filesystem::path a = MeshDirectoryNodesPath("data/profiles/prod");
  const std::filesystem::path b = MeshDirectoryNodesPath("data/profiles/qa");
  EXPECT_EQ(a.filename(), "mesh_directory_nodes.json");
  EXPECT_EQ(a.parent_path(), std::filesystem::path("data/profiles/prod"));
  EXPECT_NE(a, b);
}

} // namespace pbr
