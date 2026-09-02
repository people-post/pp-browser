#include "domain/messaging/PeerCapsLogic.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(PeerCapsLogicTest, KeepsOrgSeedsDropsUnknownContacts) {
  MeshHopCandidate contact;
  contact.peer_id = "12D3KooWPhone";
  contact.affinity = MeshHopAffinity::Contact;
  contact.dialable = true;

  MeshHopCandidate seed;
  seed.peer_id = "12D3KooWSeed";
  seed.affinity = MeshHopAffinity::OrgSeed;
  seed.dialable = true;

  MeshHopCandidate friend_node;
  friend_node.peer_id = "12D3KooWDesktop";
  friend_node.affinity = MeshHopAffinity::Contact;
  friend_node.dialable = true;

  auto ranked = FilterHopsByMediaRelayAds(
      {contact, seed, friend_node},
      [](const std::string& peer_id) { return peer_id == "12D3KooWDesktop"; });
  ASSERT_EQ(ranked.size(), 2u);
  EXPECT_EQ(ranked[0].peer_id, "12D3KooWSeed");
  EXPECT_EQ(ranked[1].peer_id, "12D3KooWDesktop");
}

TEST(PeerCapsLogicTest, KeepsDirectoryNodesWithMediaRelay) {
  MeshHopCandidate directory;
  directory.peer_id = "12D3KooWNode";
  directory.affinity = MeshHopAffinity::DirectoryNode;
  directory.advertises_media_relay = true;

  MeshHopCandidate directory_no_media;
  directory_no_media.peer_id = "12D3KooWOther";
  directory_no_media.affinity = MeshHopAffinity::DirectoryNode;
  directory_no_media.advertises_media_relay = false;

  auto ranked = FilterHopsByMediaRelayAds({directory, directory_no_media}, {});
  ASSERT_EQ(ranked.size(), 1u);
  EXPECT_EQ(ranked[0].peer_id, "12D3KooWNode");
}

TEST(PeerCapsLogicTest, KeepsDhtNodesWithMediaRelay) {
  MeshHopCandidate dht;
  dht.peer_id = "12D3KooWDht";
  dht.affinity = MeshHopAffinity::DhtDiscovered;
  dht.advertises_media_relay = true;

  MeshHopCandidate dht_no_media;
  dht_no_media.peer_id = "12D3KooWOther";
  dht_no_media.affinity = MeshHopAffinity::DhtDiscovered;
  dht_no_media.advertises_media_relay = false;

  auto ranked = FilterHopsByMediaRelayAds({dht, dht_no_media}, {});
  ASSERT_EQ(ranked.size(), 1u);
  EXPECT_EQ(ranked[0].peer_id, "12D3KooWDht");
}

TEST(PeerCapsLogicTest, NullAdsKeepsOnlySeeds) {
  MeshHopCandidate contact;
  contact.peer_id = "12D3KooWPhone";
  contact.affinity = MeshHopAffinity::Contact;

  MeshHopCandidate seed;
  seed.peer_id = "12D3KooWSeed";
  seed.affinity = MeshHopAffinity::OrgSeed;

  auto ranked = FilterHopsByMediaRelayAds({contact, seed}, {});
  ASSERT_EQ(ranked.size(), 1u);
  EXPECT_EQ(ranked[0].peer_id, "12D3KooWSeed");
}

TEST(PeerCapsLogicTest, PeerIdsFromListenMultiaddrs) {
  const auto ids = PeerIdsFromListenMultiaddrs(
      {"/ip4/10.0.0.1/tcp/18517/p2p/12D3KooWAlice", "/ip4/10.0.0.2/tcp/18517/p2p/12D3KooWAlice",
       "/ip4/10.0.0.3/tcp/18517/p2p/12D3KooWBob"});
  ASSERT_EQ(ids.size(), 2u);
  EXPECT_EQ(ids[0], "12D3KooWAlice");
  EXPECT_EQ(ids[1], "12D3KooWBob");
}

TEST(PeerCapsLogicTest, MergeAdvertisedMediaRelayHopsPrependsMissing) {
  MeshHopCandidate seed;
  seed.peer_id = "12D3KooWSeed";
  seed.affinity = MeshHopAffinity::OrgSeed;

  auto ranked = MergeAdvertisedMediaRelayHops(
      {seed}, {"12D3KooWLinux", "12D3KooWSeed"},
      [](const std::string& peer_id) {
        return peer_id == "12D3KooWLinux" ? "/ip4/10.0.0.1/tcp/18517/p2p/12D3KooWLinux" : "";
      });
  ASSERT_EQ(ranked.size(), 2u);
  EXPECT_EQ(ranked[0].peer_id, "12D3KooWLinux");
  EXPECT_EQ(ranked[0].multiaddr, "/ip4/10.0.0.1/tcp/18517/p2p/12D3KooWLinux");
  EXPECT_EQ(ranked[1].peer_id, "12D3KooWSeed");
}

} // namespace
} // namespace pbr
