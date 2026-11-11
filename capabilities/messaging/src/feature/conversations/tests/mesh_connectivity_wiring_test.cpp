#include "feature/conversations/MeshConnectivityWiring.h"

#include "lib/people/ContactsStore.h"
#include "lib/people/MeshHopPolicy.h"
#include "common/MeshConfig.h"
#include "common/runtime/AppRuntime.h"
#include "common/PbrCompat.h"
#include "common/runtime/AppRuntime.h"
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
constexpr const char* kSeedA = "12D3KooWOrgSeedA";
constexpr const char* kSeedB = "12D3KooWOrgSeedB";

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
    MeshConfig cfg; // as the app hands it over: normalized, here with two org seeds
    cfg.bootstrap_peers = OrgSeeds();
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
    in.list_dht_nodes = []() {
      // The Friendly contact's node publishes itself as a circuit relay; the plain contact is a phone.
      mesh::MeshDirectoryNode node;
      node.peer_id = kFriend;
      node.multiaddrs = {std::string("/ip4/198.51.100.8/udp/443/adp/1.0.0/p2p/") + kFriend};
      node.circuit_relay = true;
      return std::vector<mesh::MeshDirectoryNode>{node};
    };
    in.seed_dial_ok = []() { return true; };
    return in;
  }

  static std::vector<std::string> OrgSeeds() {
    return {std::string("/ip4/203.0.113.1/udp/443/adp/1.0.0/p2p/") + kSeedA,
            std::string("/ip4/203.0.113.2/udp/443/adp/1.0.0/p2p/") + kSeedB};
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
  const auto open_surface = Ids(open.Rendezvous());
  EXPECT_NE(std::find(open_surface.begin(), open_surface.end(), kVolunteer), open_surface.end());
  EXPECT_NE(std::find(open_surface.begin(), open_surface.end(), kFriend), open_surface.end());
  EXPECT_EQ(std::find(open_surface.begin(), open_surface.end(), kPlain), open_surface.end())
      << "a contact that is no circuit relay is not offered as one (#379)";

  const std::vector<std::string> seeds = Ids(CollectSeedHopCandidates(OrgSeeds()));
  ASSERT_FALSE(seeds.empty());
  std::vector<std::string> seeds_and_friend = seeds;
  seeds_and_friend.push_back(kFriend);
  std::sort(seeds_and_friend.begin(), seeds_and_friend.end());

  const mesh::MeshHopPolicy trusted = ComputeMeshHopPolicy(Inputs(true));
  EXPECT_EQ(Ids(trusted.Rendezvous()), seeds_and_friend);
  EXPECT_EQ(Ids(trusted.Seeds()), seeds) << "no directory volunteer";
  EXPECT_EQ(trusted.Introducers().contact_peer_ids, std::vector<std::string>{kFriend});
}

// Seeds a probe found unreachable are neither on the rendezvous surface nor a park target: Accept
// waits for the park, and waiting on seeds that cannot connect held every answer for 12 s.
TEST_F(MeshConnectivityWiringTest, UnreachableSeedsAreNoParkTarget) {
  const std::vector<std::string> seeds = Ids(CollectSeedHopCandidates(OrgSeeds()));
  ASSERT_FALSE(seeds.empty());
  const auto reachable = Ids(ComputeMeshHopPolicy(Inputs(false)).Seeds());
  for (const std::string& seed : seeds) {
    EXPECT_NE(std::find(reachable.begin(), reachable.end(), seed), reachable.end()) << "reachable (or never probed)";
  }

  MeshConnectivityWiringInputs in = Inputs(false);
  in.seed_dial_ok = []() { return false; };
  const mesh::MeshHopPolicy unreachable = ComputeMeshHopPolicy(in);
  EXPECT_EQ(Ids(unreachable.Seeds()), std::vector<std::string>{kVolunteer}) << "the directory's still count";
  const auto surface = Ids(unreachable.Rendezvous());
  for (const std::string& seed : seeds) {
    EXPECT_EQ(std::find(surface.begin(), surface.end(), seed), surface.end()) << seed;
  }
}

// N032 rules 5–8 on the park / circuit surface: a node whose claim was contradicted (it said a newer version than
// it runs) is demoted with its operator and goes last, a second relay of one operator comes after the others, and an
// anchor (org seed or Friendly contact's node) leads.
TEST_F(MeshConnectivityWiringTest, TheRendezvousSurfaceDemotesLiarsSpreadsOperatorsAndLeadsWithAnAnchor) {
  MeshConnectivityWiringInputs in = Inputs(false);
  auto row = [](const std::string& peer, const std::string& account, const std::string& version) {
    mesh::MeshDirectoryNode node;
    node.peer_id = peer;
    node.account_id = account;
    node.node_version = version;
    node.multiaddrs = {"/ip4/198.51.100.20/udp/443/adp/1.0.0/p2p/" + peer};
    node.circuit_relay = true;
    return node;
  };
  in.list_directory_nodes = [row]() {
    return std::vector<mesh::MeshDirectoryNode>{row("12D3KooWLiar", "account:liar", "9.9.9"),
                                                row("12D3KooWLiarTwo", "account:liar", "4.1.0"),
                                                row("12D3KooWHonest", "account:honest", "4.1.0")};
  };
  in.observe_node = [](const std::string& peer_id) {
    ObservedNode seen;
    if (peer_id == "12D3KooWLiar") { // reached: it runs 4.1.0, not the 9.9.9 it registered
      seen.told = true;
      seen.amp_wire = 2;
      seen.node_version = "4.1.0";
    }
    return seen;
  };
  auto reputation = std::make_shared<MeshNodeReputation>();
  in.node_reputation = [reputation]() { return reputation; };

  const auto surface = ComputeMeshHopPolicy(in).Rendezvous();
  std::vector<std::string> order;
  for (const auto& hop : surface) {
    order.push_back(hop.peer_id);
  }
  ASSERT_FALSE(order.empty());
  const std::vector<std::string> anchors = {kSeedA, kSeedB, kFriend};
  EXPECT_NE(std::find(anchors.begin(), anchors.end(), order.front()), anchors.end()) << "an anchor leads";
  const auto at = [&order](const std::string& id) { return std::find(order.begin(), order.end(), id) - order.begin(); };
  const auto last = static_cast<ptrdiff_t>(order.size()) - 1;
  EXPECT_GE(at("12D3KooWLiar"), last - 1) << "the liar is still offered, behind every anchor and honest node";
  EXPECT_GE(at("12D3KooWLiarTwo"), last - 1) << "its operator is demoted with it";
  EXPECT_LT(at("12D3KooWHonest"), at("12D3KooWLiar"));
  EXPECT_TRUE(reputation->IsDemoted("12D3KooWLiar", "account:liar", MeshNodeReputation::NowMs()));
  EXPECT_FALSE(reputation->IsDemoted("12D3KooWHonest", "account:honest", MeshNodeReputation::NowMs()));
}

// K015 (#577): the relays that last carried a circuit go to the park first after a start — those it may still
// park on; one no longer listed, or one this profile distrusts, does not jump ahead of the anchors.
TEST_F(MeshConnectivityWiringTest, TheLastGoodRelaysTheParkMayUseLeadTheHopPolicy) {
  MeshConnectivityWiringInputs in = Inputs(false);
  EXPECT_TRUE(ComputeMeshHopPolicy(in).last_good_relays.empty()) << "none kept: the policy's order";

  in.last_good_relays = []() { return std::vector<std::string>{kVolunteer, "12D3KooWGoneRelay", kSeedB, kPlain}; };
  EXPECT_EQ(ComputeMeshHopPolicy(in).last_good_relays, (std::vector<std::string>{kVolunteer, kSeedB}))
      << "in their order; a relay no longer listed and a contact that is no relay are dropped";

  MeshConnectivityWiringInputs trusted = Inputs(true);
  trusted.last_good_relays = in.last_good_relays;
  EXPECT_EQ(ComputeMeshHopPolicy(trusted).last_good_relays, std::vector<std::string>{kSeedB})
      << "trusted relays only: the directory's volunteer is no park target";

  auto reputation = std::make_shared<MeshNodeReputation>();
  reputation->NoteContradiction(kVolunteer, "", MeshNodeReputation::NowMs());
  in.node_reputation = [reputation]() { return reputation; };
  EXPECT_EQ(ComputeMeshHopPolicy(in).last_good_relays, std::vector<std::string>{kSeedB}) << "a demoted node";
}

// THREADING.md § Mesh threads, rule 1: the app pushes the policy; the mesh never computes it. Start
// pushes on the caller (the policy is in before connectivity wires), changes push from a worker, and
// nothing is pushed between Stop and the next Start (the inputs read a mesh that may be going).
TEST_F(MeshConnectivityWiringTest, TheFeedPushesWhileStartedAndNeverAfterStop) {
  AppRuntime::Initialize(AppRuntimeConfig{});
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
    EXPECT_TRUE(connectivity.HopPolicy()->Rendezvous().empty());

    feed.Start();
    EXPECT_EQ(directory_reads->load(), 1) << "Start pushes on the caller";
    // Contacts are punch introducers: the pushed policy shows them.
    const auto introducers = [&connectivity]() { return connectivity.HopPolicy()->Introducers().contact_peer_ids; };
    const auto surface = introducers();
    EXPECT_NE(std::find(surface.begin(), surface.end(), kPlain), surface.end());

    AddContact("c-new", "12D3KooWNewContact", TrustLevel::Unknown);
    feed.PushSoon();
    for (int i = 0; i < 200 && directory_reads->load() < 2; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_GE(directory_reads->load(), 2) << "PushSoon pushes from a worker";
    for (int i = 0; i < 200; ++i) {
      const auto now = introducers();
      if (std::find(now.begin(), now.end(), "12D3KooWNewContact") != now.end()) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const auto updated = introducers();
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
