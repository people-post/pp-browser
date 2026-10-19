#include "domain/messaging/PeerCapsLogic.h"
#include "common/MeshAlias.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(PeerCapsLogicTest, KeepsOrgSeedsDropsUnknownContacts) {
  mesh::MeshHopCandidate contact;
  contact.peer_id = "12D3KooWPhone";
  contact.affinity = mesh::MeshHopAffinity::Contact;
  contact.dialable = true;

  mesh::MeshHopCandidate seed;
  seed.peer_id = "12D3KooWSeed";
  seed.affinity = mesh::MeshHopAffinity::OrgSeed;
  seed.dialable = true;

  mesh::MeshHopCandidate friend_node;
  friend_node.peer_id = "12D3KooWDesktop";
  friend_node.affinity = mesh::MeshHopAffinity::Contact;
  friend_node.dialable = true;

  auto ranked = FilterHopsByMediaRelayAds(
      {contact, seed, friend_node},
      [](const std::string& peer_id) { return peer_id == "12D3KooWDesktop"; });
  ASSERT_EQ(ranked.size(), 2u);
  EXPECT_EQ(ranked[0].peer_id, "12D3KooWSeed");
  EXPECT_EQ(ranked[1].peer_id, "12D3KooWDesktop");
}

TEST(PeerCapsLogicTest, KeepsDirectoryNodesWithMediaRelay) {
  mesh::MeshHopCandidate directory;
  directory.peer_id = "12D3KooWNode";
  directory.affinity = mesh::MeshHopAffinity::DirectoryNode;
  directory.advertises_media_relay = true;

  mesh::MeshHopCandidate directory_no_media;
  directory_no_media.peer_id = "12D3KooWOther";
  directory_no_media.affinity = mesh::MeshHopAffinity::DirectoryNode;
  directory_no_media.advertises_media_relay = false;

  auto ranked = FilterHopsByMediaRelayAds({directory, directory_no_media}, {});
  ASSERT_EQ(ranked.size(), 1u);
  EXPECT_EQ(ranked[0].peer_id, "12D3KooWNode");
}

TEST(PeerCapsLogicTest, KeepsDhtNodesWithMediaRelay) {
  mesh::MeshHopCandidate dht;
  dht.peer_id = "12D3KooWDht";
  dht.affinity = mesh::MeshHopAffinity::DhtDiscovered;
  dht.advertises_media_relay = true;

  mesh::MeshHopCandidate dht_no_media;
  dht_no_media.peer_id = "12D3KooWOther";
  dht_no_media.affinity = mesh::MeshHopAffinity::DhtDiscovered;
  dht_no_media.advertises_media_relay = false;

  auto ranked = FilterHopsByMediaRelayAds({dht, dht_no_media}, {});
  ASSERT_EQ(ranked.size(), 1u);
  EXPECT_EQ(ranked[0].peer_id, "12D3KooWDht");
}

TEST(PeerCapsLogicTest, NullAdsKeepsOnlySeeds) {
  mesh::MeshHopCandidate contact;
  contact.peer_id = "12D3KooWPhone";
  contact.affinity = mesh::MeshHopAffinity::Contact;

  mesh::MeshHopCandidate seed;
  seed.peer_id = "12D3KooWSeed";
  seed.affinity = mesh::MeshHopAffinity::OrgSeed;

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
  mesh::MeshHopCandidate seed;
  seed.peer_id = "12D3KooWSeed";
  seed.affinity = mesh::MeshHopAffinity::OrgSeed;

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

TEST(PeerCapsLogicTest, DirectoryMediaRelayLookupAndMerge) {
  mesh::MeshDirectoryNode with_relay;
  with_relay.peer_id = "12D3KooWNode";
  with_relay.media_relay = true;
  mesh::MeshDirectoryNode no_relay;
  no_relay.peer_id = "12D3KooWOther";
  no_relay.media_relay = false;
  mesh::MeshDirectoryNode dht_relay;
  dht_relay.peer_id = "12D3KooWDht";
  dht_relay.media_relay = true;

  EXPECT_TRUE(DirectoryNodeAdvertisesMediaRelay(with_relay));
  EXPECT_FALSE(DirectoryNodeAdvertisesMediaRelay(no_relay));
  EXPECT_TRUE(PeerHasMediaRelayInDirectory("12D3KooWNode", {with_relay, no_relay}));
  EXPECT_FALSE(PeerHasMediaRelayInDirectory("12D3KooWOther", {with_relay, no_relay}));

  const auto merged =
      MergeMediaRelayCapablePeerIds({"12D3KooWCallCap", "12D3KooWNode"}, {with_relay, no_relay}, {dht_relay});
  ASSERT_EQ(merged.size(), 3u);
  EXPECT_EQ(merged[0], "12D3KooWCallCap");
  EXPECT_EQ(merged[1], "12D3KooWNode");
  EXPECT_EQ(merged[2], "12D3KooWDht");
}

} // namespace
} // namespace pbr
