#include "feature/conversations/MeshConnectivityWiring.h"

#include "domain/people/ContactsStore.h"
#include "domain/people/MeshHopPolicy.h"
#include "foundation/data/MeshRole.h"
#include "foundation/runtime/AppRuntime.h"
#include "common/MeshAlias.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace pbr {
namespace {

constexpr const char* kFriend = "12D3KooWFriendNode";
constexpr const char* kPlain = "12D3KooWPlainContact";
constexpr const char* kVolunteer = "12D3KooWDirectoryVolunteer";

class MeshConnectivityWiringTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("pp_mesh_wiring_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    contacts_ = std::make_unique<ContactsStore>(dir_.string());
    AddContact("c-friend", kFriend, TrustLevel::Friendly);
    AddContact("c-plain", kPlain, TrustLevel::Unknown);
  }
  void TearDown() override {
    contacts_.reset();
    std::filesystem::remove_all(dir_);
  }

  void AddContact(const std::string& id, const std::string& peer_id, const TrustLevel trust) {
    Contact contact;
    contact.id = id;
    contact.local.trust = trust;
    contact.remote.ids = {{mesh::ContactIdKind::PeerId, peer_id, true}};
    contact.remote.multiaddrs = {"/ip4/10.0.0.9/udp/4001/adp/1.0.0/p2p/" + peer_id};
    SyncContactMirrors(contact);
    ASSERT_TRUE(contacts_->Upsert(contact));
  }

  MeshConnectivityWiringInputs Inputs(const bool trusted_only) {
    MeshConfig cfg;  // the org seeds are whatever normalization makes of the configured list
    cfg.trusted_relays_only = trusted_only;
    MeshConnectivityWiringInputs in;
    in.contacts = contacts_.get();
    in.mesh_config = [cfg]() { return std::make_shared<const MeshConfig>(cfg); };
    in.list_directory_nodes = []() {
      mesh::MeshDirectoryNode node;
      node.peer_id = kVolunteer;
      node.multiaddrs = {std::string("/ip4/198.51.100.7/udp/443/adp/1.0.0/p2p/") + kVolunteer};
      node.circuit_relay = true;
      return std::vector<mesh::MeshDirectoryNode>{node};
    };
    in.seed_dial_ok = []() { return true; };
    return in;
  }

  static std::vector<std::string> Ids(const std::vector<mesh::MeshHopCandidate>& hops) {
    std::vector<std::string> out;
    for (const auto& hop : hops) {
      out.push_back(hop.peer_id);
    }
    std::sort(out.begin(), out.end());
    return out;
  }

  std::filesystem::path dir_;
  std::unique_ptr<ContactsStore> contacts_;
};

// projects/privacy T4: with trusted relays only, the rendezvous surface, the bootstrap seeds and the
// punch introducers hold org seeds and Friendly contacts' nodes — nobody else sees both our addresses.
TEST_F(MeshConnectivityWiringTest, TrustedRelaysOnlyNarrowsEveryRelayRole) {
  const mesh::MeshHopPolicy open = ComputeMeshHopPolicy(Inputs(false));
  const auto open_surface = Ids(open.rendezvous_candidates);
  EXPECT_NE(std::find(open_surface.begin(), open_surface.end(), kVolunteer), open_surface.end());
  EXPECT_NE(std::find(open_surface.begin(), open_surface.end(), kPlain), open_surface.end());

  MeshConfig normalized;
  NormalizeMeshConfig(normalized);
  const std::vector<std::string> seeds = Ids(CollectSeedHopCandidates(normalized.bootstrap_peers));
  ASSERT_FALSE(seeds.empty());
  std::vector<std::string> seeds_and_friend = seeds;
  seeds_and_friend.push_back(kFriend);
  std::sort(seeds_and_friend.begin(), seeds_and_friend.end());

  const mesh::MeshHopPolicy trusted = ComputeMeshHopPolicy(Inputs(true));
  EXPECT_EQ(Ids(trusted.rendezvous_candidates), seeds_and_friend);
  EXPECT_EQ(Ids(trusted.bootstrap_seeds), seeds) << "no directory volunteer";
  EXPECT_EQ(trusted.punch_introducers.contact_peer_ids, std::vector<std::string>{kFriend});
}

// THREADING.md § Mesh threads, rule 1: the app pushes the policy; the mesh never computes it. Start
// pushes on the caller (the policy is in before connectivity wires), changes push from a worker, and
// nothing is pushed between Stop and the next Start (the inputs read a mesh that may be going).
TEST_F(MeshConnectivityWiringTest, TheFeedPushesWhileStartedAndNeverAfterStop) {
  AppRuntime::Initialize();
  {
    mesh::MeshConnectivity connectivity;
    MeshHopPolicyFeed feed(connectivity);
    MeshConnectivityWiringInputs in = Inputs(false);
    auto directory_reads = std::make_shared<std::atomic<int>>(0);
    in.list_directory_nodes = [directory_reads]() {
      ++*directory_reads;
      return std::vector<mesh::MeshDirectoryNode>{};
    };
    feed.SetInputs(std::move(in));
    EXPECT_TRUE(connectivity.HopPolicy()->rendezvous_candidates.empty());

    feed.Start();
    EXPECT_EQ(directory_reads->load(), 1) << "Start pushes on the caller";
    const auto surface = Ids(connectivity.HopPolicy()->rendezvous_candidates);
    EXPECT_NE(std::find(surface.begin(), surface.end(), kPlain), surface.end());

    AddContact("c-new", "12D3KooWNewContact", TrustLevel::Unknown);
    feed.PushSoon();
    for (int i = 0; i < 200 && directory_reads->load() < 2; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_GE(directory_reads->load(), 2) << "PushSoon pushes from a worker";
    for (int i = 0; i < 200; ++i) {
      const auto now = Ids(connectivity.HopPolicy()->rendezvous_candidates);
      if (std::find(now.begin(), now.end(), "12D3KooWNewContact") != now.end()) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const auto updated = Ids(connectivity.HopPolicy()->rendezvous_candidates);
    EXPECT_NE(std::find(updated.begin(), updated.end(), "12D3KooWNewContact"), updated.end());

    feed.Stop();
    const int at_stop = directory_reads->load();
    feed.PushSoon();
    EXPECT_TRUE(AppRuntime::DrainWorkersThenUI());
    EXPECT_EQ(directory_reads->load(), at_stop) << "nothing is computed while stopped";
  }
  AppRuntime::Shutdown();
}

} // namespace
} // namespace pbr
