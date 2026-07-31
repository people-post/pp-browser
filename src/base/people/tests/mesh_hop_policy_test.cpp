#include "base/people/MeshHopPolicy.h"
#include "libp2p/integration/host/MediaRelayService.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

Contact MakeContact(const std::string& peer_id, const std::string& ma = {}) {
  Contact c;
  c.id = "c-" + peer_id;
  c.display_name = peer_id;
  c.ids.push_back({ContactIdKind::PeerId, peer_id, true});
  if (!ma.empty()) {
    c.multiaddrs.push_back(ma);
  }
  return c;
}

TEST(MeshHopPolicyTest, CircuitOrdersContactsBeforeSeeds) {
  auto contacts = CollectContactHopCandidates({MakeContact("12D3KooWFriend")});
  auto seeds = CollectSeedHopCandidates(
      {"/ip4/1.2.3.4/tcp/443/p2p/12D3KooWCmqCKgBL47m25WzUgiAPayf3GqKiRosmPvAqp2MQUFYR"});
  auto ordered = OrderCircuitHops(contacts, seeds, true);
  ASSERT_EQ(ordered.size(), 2u);
  EXPECT_EQ(ordered[0].peer_id, "12D3KooWFriend");
  EXPECT_EQ(ordered[0].affinity, MeshHopAffinity::Contact);
  EXPECT_EQ(ordered[1].affinity, MeshHopAffinity::OrgSeed);
}

TEST(MeshHopPolicyTest, MediaRankSkipsFailedAndPrefersCapacityPlusAffinity) {
  MeshHopCandidate friend_hop;
  friend_hop.peer_id = "friend";
  friend_hop.affinity = MeshHopAffinity::Contact;
  friend_hop.residual_capacity = 0.4;

  MeshHopCandidate seed_hop;
  seed_hop.peer_id = "seed";
  seed_hop.affinity = MeshHopAffinity::OrgSeed;
  seed_hop.residual_capacity = 0.9;

  MeshHopCandidate failed;
  failed.peer_id = "bad";
  failed.affinity = MeshHopAffinity::Contact;
  failed.recently_failed = true;
  failed.residual_capacity = 1.0;

  MeshHopCandidate stranger;
  stranger.peer_id = "pub";
  stranger.affinity = MeshHopAffinity::Other;
  stranger.residual_capacity = 1.0;

  auto ranked = RankMediaHops({failed, stranger, friend_hop, seed_hop}, true);
  ASSERT_EQ(ranked.size(), 2u);
  // Seed has more residual capacity; affinity bonus should still keep friend competitive,
  // but 0.9*100+10+5=105 vs 0.4*100+25+5=70 → seed first.
  EXPECT_EQ(ranked[0].peer_id, "seed");
  EXPECT_EQ(ranked[1].peer_id, "friend");
}

TEST(MeshHopPolicyTest, MediaRankPreferContactsOffPutsSeedsFirst) {
  MeshHopCandidate friend_hop;
  friend_hop.peer_id = "friend";
  friend_hop.affinity = MeshHopAffinity::Contact;
  friend_hop.residual_capacity = 1.0;

  MeshHopCandidate seed_hop;
  seed_hop.peer_id = "seed";
  seed_hop.affinity = MeshHopAffinity::OrgSeed;
  seed_hop.residual_capacity = 1.0;

  auto ranked = RankMediaHops({friend_hop, seed_hop}, false);
  ASSERT_EQ(ranked.size(), 2u);
  EXPECT_EQ(ranked[0].peer_id, "seed");
  EXPECT_EQ(ranked[1].peer_id, "friend");
}

TEST(MeshHopPolicyTest, CircuitPreferContactsOffPutsSeedsFirst) {
  auto contacts = CollectContactHopCandidates({MakeContact("12D3KooWFriend")});
  auto seeds = CollectSeedHopCandidates(
      {"/ip4/1.2.3.4/tcp/443/p2p/12D3KooWCmqCKgBL47m25WzUgiAPayf3GqKiRosmPvAqp2MQUFYR"});
  auto ordered = OrderCircuitHops(contacts, seeds, false);
  ASSERT_EQ(ordered.size(), 2u);
  EXPECT_EQ(ordered[0].affinity, MeshHopAffinity::OrgSeed);
  EXPECT_EQ(ordered[1].peer_id, "12D3KooWFriend");
}

TEST(MeshHopPolicyTest, ContactPeerIdHelpers) {
  const std::vector<Contact> contacts = {MakeContact("12D3KooWFriend")};
  EXPECT_TRUE(IsContactPeerId(contacts, "12D3KooWFriend"));
  EXPECT_FALSE(IsContactPeerId(contacts, "other"));
  EXPECT_EQ(ContactPeerIds(contacts).size(), 1u);
}

TEST(MeshHopPolicyTest, ExcludeSelfHopDropsLocalPeerId) {
  MeshHopCandidate self_hop;
  self_hop.peer_id = "12D3KooWSelf";
  self_hop.affinity = MeshHopAffinity::Contact;
  MeshHopCandidate other;
  other.peer_id = "12D3KooWOther";
  other.affinity = MeshHopAffinity::OrgSeed;

  auto filtered = ExcludeSelfHop({self_hop, other}, "12D3KooWSelf");
  ASSERT_EQ(filtered.size(), 1u);
  EXPECT_EQ(filtered[0].peer_id, "12D3KooWOther");

  EXPECT_EQ(ExcludeSelfHop({self_hop, other}, "").size(), 2u);
}

TEST(MeshHopPolicyTest, PreferInCallMediaHopsPutsCallMemberFirst) {
  MeshHopCandidate linux_out;
  linux_out.peer_id = "12D3KooWLinux";
  linux_out.multiaddr = "/ip4/1.1.1.1/tcp/1/p2p/12D3KooWLinux";
  linux_out.affinity = MeshHopAffinity::Contact;

  MeshHopCandidate windows_in;
  windows_in.peer_id = "12D3KooWWin";
  windows_in.multiaddr = "/ip4/2.2.2.2/tcp/1/p2p/12D3KooWWin";
  windows_in.affinity = MeshHopAffinity::Contact;

  MeshHopCandidate seed;
  seed.peer_id = "12D3KooWSeed";
  seed.multiaddr = "/ip4/3.3.3.3/tcp/1/p2p/12D3KooWSeed";
  seed.affinity = MeshHopAffinity::OrgSeed;

  // Seed-first ranking (prefer contacts off), then boost in-call Windows ahead.
  auto ranked = PreferInCallMediaHops({seed, linux_out, windows_in}, {"12D3KooWWin"});
  ASSERT_EQ(ranked.size(), 3u);
  EXPECT_EQ(ranked[0].peer_id, "12D3KooWWin");
  EXPECT_EQ(ranked[1].peer_id, "12D3KooWSeed");
  EXPECT_EQ(ranked[2].peer_id, "12D3KooWLinux");
}

TEST(MediaFrameCodecTest, RoundTripHeaderAndPayload) {
  MediaDataFrame in;
  in.stream_id = 42;
  in.channel_id = 7;
  in.channel_type = MediaChannelType::LatestLossy;
  in.seq = 99;
  in.mark = 1;
  in.payload = {1, 2, 3, 4, 5};
  const auto bytes = EncodeMediaDataFrame(in);
  auto out = DecodeMediaDataFrame(bytes);
  ASSERT_TRUE(out) << out.error().message;
  EXPECT_EQ(out->stream_id, 42u);
  EXPECT_EQ(out->channel_id, 7u);
  EXPECT_EQ(out->channel_type, MediaChannelType::LatestLossy);
  EXPECT_EQ(out->seq, 99u);
  EXPECT_EQ(out->mark, 1);
  EXPECT_EQ(out->payload, in.payload);
}

} // namespace
} // namespace pbr
