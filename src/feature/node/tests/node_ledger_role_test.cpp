#include "feature/node/NodeLedgerRole.h"

#include "mesh/testing/mesh_harness_support.h"
#include "ledger/network/AmpLedgerServer.h"

#include "amp/L1/Clock.h"
#include "amp/L1/MemoryDatagramIo.h"
#include "amp/link/AdpMultiaddr.h"
#include "amp/link/AmpStack.h"
#include "crypto/MlDsa.h"
#include "crypto/PeerId.h"

#include <gtest/gtest.h>
#include <sodium.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

using namespace pbr;
using namespace std::chrono_literals;

// pp-ledger platform-integration pi4: pp-node runs a ledger relay or miner on its mesh runtime, with
// the node's identity, and advertises the ledger's protocols only while their handlers are bound.

namespace {

std::unique_ptr<pp::amp::AmpStack> MakeStack(const std::shared_ptr<pp::adp::MemoryDatagramHub>& hub,
                                             const pp::adp::IpEndpoint& address, std::string* listen) {
  auto keys = pp::MlDsa::GenerateKeyPair();
  if (!keys) {
    return nullptr;
  }
  pp::amp::MshIdentity identity;
  identity.ml_dsa_secret_key = std::move(keys->secret_key);
  identity.ml_dsa_public_key = std::move(keys->public_key);
  auto peer_id = pp::PeerIdFromMlDsaPublicKey(identity.ml_dsa_public_key);
  if (!peer_id) {
    return nullptr;
  }
  pp::amp::AmpStack::Config config;
  config.identity = std::move(identity);
  config.local_peer_id = *peer_id;
  config.link_config = mesh::test::AmpMeshTestLinkConfig();
  auto io = std::make_shared<pp::adp::MemoryDatagramIo>(hub, address);
  auto stack = pp::amp::AmpStack::Create(io, std::make_shared<pp::adp::WallClock>(), config);
  if (!stack) {
    return nullptr;
  }
  *listen = pp::amp::FormatAdpMultiaddr(address, *peer_id).value();
  return std::move(*stack);
}

class NodeLedgerRoleTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_GE(sodium_init(), 0);
    dir_ = std::filesystem::temp_directory_path() /
           ("pp-node-ledger-role-test-" + std::to_string(std::random_device{}()));
    auto stack = MakeStack(hub_, pp::adp::IpEndpoint::V4(10, 6, 0, 1, 1000), &listen_);
    ASSERT_NE(stack, nullptr);
    // Its own MeshPump, as in pp-node: the ledger's thread posts onto a driven io lane.
    ASSERT_TRUE(static_cast<bool>(mesh_.AttachAmpStack(std::move(stack), listen_, mesh::MeshHost::AttachDrive::MeshPump)));
  }
  void TearDown() override {
    mesh_.Stop();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }
  /** Whether the mesh advertises every one of the ledger's protocols (read on its io lane). */
  bool AdvertisesTheLedger() {
    std::vector<std::string> advertised;
    mesh_.Amp()->Runtime().WithIoLock([&]() { advertised = mesh_.Amp()->Links().LocalCapability().protocols; });
    for (const std::string& id : pp::network::AmpLedgerServer::protocolIds()) {
      if (std::find(advertised.begin(), advertised.end(), id) == advertised.end()) {
        return false;
      }
    }
    return true;
  }
  /** A relay whose beacon never answers: it binds, then waits on its upstream until stopped. */
  void WriteRelayConfig() {
    std::filesystem::create_directories(dir_);
    std::string nobody;
    ASSERT_NE(MakeStack(hub_, pp::adp::IpEndpoint::V4(10, 6, 0, 9, 9), &nobody), nullptr);
    std::ofstream(dir_ / "config.json") << R"({"port": 18519, "beacon": ")" << nobody << R"("})";
  }

  std::shared_ptr<pp::adp::MemoryDatagramHub> hub_ = pp::adp::MemoryDatagramIo::MakeHub();
  std::filesystem::path dir_;
  std::string listen_;
  mesh::MeshHost mesh_{mesh::MeshExecutors{}, MetricsRegistry::Global()};
};

} // namespace

TEST_F(NodeLedgerRoleTest, ARelayAdvertisesTheLedgerWhileBoundAndWithdrawsItAtStop) {
  WriteRelayConfig();
  NodeLedgerRole ledger;
  ASSERT_TRUE(static_cast<bool>(ledger.Start(mesh_, {NodeLedgerRole::Role::Relay, dir_.string()})));

  const auto until = std::chrono::steady_clock::now() + 10s;
  while (!ledger.Listening() && std::chrono::steady_clock::now() < until) {
    std::this_thread::sleep_for(10ms);
  }
  ASSERT_TRUE(ledger.Listening());
  EXPECT_TRUE(AdvertisesTheLedger());
  std::ifstream in(dir_ / "listen.multiaddr");
  std::string listen;
  std::getline(in, listen);
  EXPECT_EQ(listen, listen_) << "the node's address and identity";
  EXPECT_FALSE(ledger.Failure().has_value());

  const auto stop_at = std::chrono::steady_clock::now();
  ledger.Stop();
  EXPECT_LT(std::chrono::steady_clock::now() - stop_at, 5s) << "shutdown does not wait out the startup";
  EXPECT_FALSE(ledger.Listening());
  EXPECT_FALSE(AdvertisesTheLedger()) << "withdrawn before the handlers went";
  EXPECT_FALSE(ledger.Failure().has_value()) << "a requested stop is not a failure";
}

TEST_F(NodeLedgerRoleTest, AStoppedMeshTakesNoRole) {
  mesh_.Stop();
  NodeLedgerRole ledger;
  EXPECT_FALSE(static_cast<bool>(ledger.Start(mesh_, {NodeLedgerRole::Role::Relay, dir_.string()})));
}

TEST(NodeLedgerRoleNamesTest, RelayOrMinerOnly) {
  EXPECT_EQ(NodeLedgerRole::RoleNamed("relay"), std::optional<NodeLedgerRole::Role>(NodeLedgerRole::Role::Relay));
  EXPECT_EQ(NodeLedgerRole::RoleNamed("miner"), std::optional<NodeLedgerRole::Role>(NodeLedgerRole::Role::Miner));
  EXPECT_FALSE(NodeLedgerRole::RoleNamed("beacon").has_value()) << "a beacon runs standalone only";
  EXPECT_FALSE(NodeLedgerRole::RoleNamed("").has_value());
}
