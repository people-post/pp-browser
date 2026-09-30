#include "feature/conversations/MeshConnectivityWiring.h"

#include "domain/people/ContactsStore.h"
#include "domain/people/MeshHopPolicy.h"
#include "foundation/data/MeshRole.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
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
    contact.remote.ids = {{ContactIdKind::PeerId, peer_id, true}};
    contact.remote.multiaddrs = {"/ip4/10.0.0.9/udp/4001/adp/1.0.0/p2p/" + peer_id};
    SyncContactMirrors(contact);
    ASSERT_TRUE(contacts_->Upsert(contact));
  }

  MeshConnectivityDeps Deps(const bool trusted_only) {
    MeshConfig cfg;  // the org seeds are whatever normalization makes of the configured list
    cfg.trusted_relays_only = trusted_only;
    MeshConnectivityWiringInputs in;
    in.contacts = contacts_.get();
    in.mesh_config = [cfg]() { return std::make_shared<const MeshConfig>(cfg); };
    in.list_directory_nodes = []() {
      MeshDirectoryNode node;
      node.peer_id = kVolunteer;
      node.multiaddrs = {std::string("/ip4/198.51.100.7/udp/443/adp/1.0.0/p2p/") + kVolunteer};
      node.circuit_relay = true;
      return std::vector<MeshDirectoryNode>{node};
    };
    in.seed_dial_ok = []() { return true; };
    return MakeMeshConnectivityDeps(std::move(in));
  }

  static std::vector<std::string> Ids(const std::vector<MeshHopCandidate>& hops) {
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
  const MeshConnectivityDeps open = Deps(false);
  const auto open_surface = Ids(open.rendezvous_candidates());
  EXPECT_NE(std::find(open_surface.begin(), open_surface.end(), kVolunteer), open_surface.end());
  EXPECT_NE(std::find(open_surface.begin(), open_surface.end(), kPlain), open_surface.end());

  MeshConfig normalized;
  NormalizeMeshConfig(normalized);
  const std::vector<std::string> seeds = Ids(CollectSeedHopCandidates(normalized.bootstrap_peers));
  ASSERT_FALSE(seeds.empty());
  std::vector<std::string> seeds_and_friend = seeds;
  seeds_and_friend.push_back(kFriend);
  std::sort(seeds_and_friend.begin(), seeds_and_friend.end());

  const MeshConnectivityDeps trusted = Deps(true);
  EXPECT_EQ(Ids(trusted.rendezvous_candidates()), seeds_and_friend);
  EXPECT_EQ(Ids(trusted.bootstrap_seeds()), seeds) << "no directory volunteer";
  const MeshPunchIntroducers introducers = trusted.punch_introducers();
  EXPECT_EQ(introducers.contact_peer_ids, std::vector<std::string>{kFriend});
}

} // namespace
} // namespace pbr
