#include "domain/people/MeshHopPolicy.h"
#include "mesh/types/RelayScope.h"
#include "common/MeshAlias.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

Contact MakeContact(const std::string& peer_id, const std::string& ma = {}) {
  Contact c;
  c.id = "c-" + peer_id;
  c.display_name = peer_id;
  c.ids.push_back({mesh::ContactIdKind::PeerId, peer_id, true});
  if (!ma.empty()) {
    c.multiaddrs.push_back(ma);
  }
  return c;
}

TEST(MeshHopPolicyTest, ProviderServeScopeMaskRequiresNode) {
  EXPECT_EQ(ProviderServeScopeMask(mesh::MeshReachabilityClass::OutboundOnly, false), 0u);
  EXPECT_EQ(ProviderServeScopeMask(mesh::MeshReachabilityClass::Reachable, true), mesh::kRelayScopeVolunteerServe);
}

TEST(MeshHopPolicyTest, EscalatingPrefersSameSubnetContactBeforeSeed) {
  mesh::MeshHopCandidate lan_friend;
  lan_friend.peer_id = "friend";
  lan_friend.multiaddr = "/ip4/192.168.1.50/tcp/18517/p2p/friend";
  lan_friend.affinity = mesh::MeshHopAffinity::Contact;
  lan_friend.residual_capacity = 0.5;

  mesh::MeshHopCandidate seed_hop;
  seed_hop.peer_id = "seed";
  seed_hop.multiaddr = "/ip4/1.2.3.4/tcp/443/p2p/seed";
  seed_hop.affinity = mesh::MeshHopAffinity::OrgSeed;
  seed_hop.residual_capacity = 1.0;

  const std::string listen = "/ip4/192.168.1.10/tcp/18517";
  auto ranked = RankMediaHopsEscalating({seed_hop, lan_friend}, true, listen);
  ASSERT_EQ(ranked.size(), 2u);
  EXPECT_EQ(ranked[0].peer_id, "friend");
  EXPECT_EQ(ranked[1].peer_id, "seed");
}

TEST(MeshHopPolicyTest, CandidateRelayScopesLinkOnSameSubnet) {
  mesh::MeshHopCandidate c;
  c.affinity = mesh::MeshHopAffinity::Contact;
  c.multiaddr = "/ip4/10.0.0.5/tcp/1/p2p/x";
  const auto scopes = CandidateRelayScopes(c, "/ip4/10.0.0.1/tcp/18517");
  EXPECT_TRUE(mesh::RelayScopeMaskHas(scopes, mesh::RelayScope::Link));
  EXPECT_TRUE(mesh::RelayScopeMaskHas(scopes, mesh::RelayScope::Site));
  EXPECT_TRUE(mesh::RelayScopeMaskHas(scopes, mesh::RelayScope::Social));
}

TEST(MeshHopPolicyTest, CircuitOrdersContactsBeforeSeeds) {
  auto contacts = CollectContactHopCandidates({MakeContact("12D3KooWFriend")});
  auto seeds = CollectSeedHopCandidates(
      {"/ip4/1.2.3.4/tcp/443/p2p/12D3KooWCmqCKgBL47m25WzUgiAPayf3GqKiRosmPvAqp2MQUFYR"});
  auto ordered = OrderCircuitHops(contacts, {}, {}, seeds, true);
  ASSERT_EQ(ordered.size(), 2u);
  EXPECT_EQ(ordered[0].peer_id, "12D3KooWFriend");
  EXPECT_EQ(ordered[0].affinity, mesh::MeshHopAffinity::Contact);
  EXPECT_EQ(ordered[1].affinity, mesh::MeshHopAffinity::OrgSeed);
}

TEST(MeshHopPolicyTest, CircuitOrdersContactsDirectoryThenSeeds) {
  auto contacts = CollectContactHopCandidates({MakeContact("12D3KooWFriend")});
  mesh::MeshDirectoryNode dir;
  dir.peer_id = "12D3KooWNode";
  dir.multiaddrs = {"/ip4/9.9.9.9/udp/443/adp/1.0.0/p2p/12D3KooWNode"};
  dir.media_relay = true;
  auto directory = CollectDirectoryHopCandidates({dir});
  auto seeds = CollectSeedHopCandidates(
      {"/ip4/1.2.3.4/tcp/443/p2p/12D3KooWCmqCKgBL47m25WzUgiAPayf3GqKiRosmPvAqp2MQUFYR"});
  auto ordered = OrderCircuitHops(contacts, directory, {}, seeds, true);
  ASSERT_EQ(ordered.size(), 3u);
  EXPECT_EQ(ordered[0].peer_id, "12D3KooWFriend");
  EXPECT_EQ(ordered[1].peer_id, "12D3KooWNode");
  EXPECT_EQ(ordered[1].affinity, mesh::MeshHopAffinity::DirectoryNode);
  EXPECT_EQ(ordered[2].affinity, mesh::MeshHopAffinity::OrgSeed);
}

TEST(MeshHopPolicyTest, BuildCircuitHopListSkipsSeedsWhenRequested) {
  mesh::MeshDirectoryNode dir;
  dir.peer_id = "12D3KooWNode";
  dir.multiaddrs = {"/ip4/9.9.9.9/udp/443/adp/1.0.0/p2p/12D3KooWNode"};
  auto hops = BuildCircuitHopList({}, {dir}, {},
                                  {"/ip4/1.2.3.4/tcp/443/p2p/12D3KooWCmqCKgBL47m25WzUgiAPayf3GqKiRosmPvAqp2MQUFYR"},
                                  true, false);
  ASSERT_EQ(hops.size(), 1u);
  EXPECT_EQ(hops[0].affinity, mesh::MeshHopAffinity::DirectoryNode);
}

TEST(MeshHopPolicyTest, MediaRankSkipsFailedAndPrefersCapacityPlusAffinity) {
  mesh::MeshHopCandidate friend_hop;
  friend_hop.peer_id = "friend";
  friend_hop.affinity = mesh::MeshHopAffinity::Contact;
  friend_hop.residual_capacity = 0.4;

  mesh::MeshHopCandidate seed_hop;
  seed_hop.peer_id = "seed";
  seed_hop.affinity = mesh::MeshHopAffinity::OrgSeed;
  seed_hop.residual_capacity = 0.9;

  mesh::MeshHopCandidate failed;
  failed.peer_id = "bad";
  failed.affinity = mesh::MeshHopAffinity::Contact;
  failed.recently_failed = true;
  failed.residual_capacity = 1.0;

  mesh::MeshHopCandidate stranger;
  stranger.peer_id = "pub";
  stranger.affinity = mesh::MeshHopAffinity::Other;
  stranger.residual_capacity = 1.0;

  auto ranked = RankMediaHops({failed, stranger, friend_hop, seed_hop}, true);
  ASSERT_EQ(ranked.size(), 2u);
  // Seed has more residual capacity; affinity bonus should still keep friend competitive,
  // but 0.9*100+10+5=105 vs 0.4*100+25+5=70 → seed first.
  EXPECT_EQ(ranked[0].peer_id, "seed");
  EXPECT_EQ(ranked[1].peer_id, "friend");
}

TEST(MeshHopPolicyTest, MediaRankPreferContactsOffPutsSeedsFirst) {
  mesh::MeshHopCandidate friend_hop;
  friend_hop.peer_id = "friend";
  friend_hop.affinity = mesh::MeshHopAffinity::Contact;
  friend_hop.residual_capacity = 1.0;

  mesh::MeshHopCandidate seed_hop;
  seed_hop.peer_id = "seed";
  seed_hop.affinity = mesh::MeshHopAffinity::OrgSeed;
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
  auto ordered = OrderCircuitHops(contacts, {}, {}, seeds, false);
  ASSERT_EQ(ordered.size(), 2u);
  EXPECT_EQ(ordered[0].affinity, mesh::MeshHopAffinity::OrgSeed);
  EXPECT_EQ(ordered[1].peer_id, "12D3KooWFriend");
}

TEST(MeshHopPolicyTest, ContactPeerIdHelpers) {
  const std::vector<Contact> contacts = {MakeContact("12D3KooWFriend")};
  EXPECT_TRUE(IsContactPeerId(contacts, "12D3KooWFriend"));
  EXPECT_FALSE(IsContactPeerId(contacts, "other"));
  EXPECT_EQ(ContactPeerIds(contacts).size(), 1u);
}

TEST(MeshHopPolicyTest, ExcludeSelfHopDropsLocalPeerId) {
  mesh::MeshHopCandidate self_hop;
  self_hop.peer_id = "12D3KooWSelf";
  self_hop.affinity = mesh::MeshHopAffinity::Contact;
  mesh::MeshHopCandidate other;
  other.peer_id = "12D3KooWOther";
  other.affinity = mesh::MeshHopAffinity::OrgSeed;

  auto filtered = ExcludeSelfHop({self_hop, other}, "12D3KooWSelf");
  ASSERT_EQ(filtered.size(), 1u);
  EXPECT_EQ(filtered[0].peer_id, "12D3KooWOther");

  EXPECT_EQ(ExcludeSelfHop({self_hop, other}, "").size(), 2u);
}

TEST(MeshHopPolicyTest, PreferInCallMediaHopsPutsCallMemberFirst) {
  mesh::MeshHopCandidate linux_out;
  linux_out.peer_id = "12D3KooWLinux";
  linux_out.multiaddr = "/ip4/1.1.1.1/tcp/1/p2p/12D3KooWLinux";
  linux_out.affinity = mesh::MeshHopAffinity::Contact;

  mesh::MeshHopCandidate windows_in;
  windows_in.peer_id = "12D3KooWWin";
  windows_in.multiaddr = "/ip4/2.2.2.2/tcp/1/p2p/12D3KooWWin";
  windows_in.affinity = mesh::MeshHopAffinity::Contact;

  mesh::MeshHopCandidate seed;
  seed.peer_id = "12D3KooWSeed";
  seed.multiaddr = "/ip4/3.3.3.3/tcp/1/p2p/12D3KooWSeed";
  seed.affinity = mesh::MeshHopAffinity::OrgSeed;

  // Seed-first ranking (prefer contacts off), then boost in-call Windows ahead.
  auto ranked = PreferInCallMediaHops({seed, linux_out, windows_in}, {"12D3KooWWin"});
  ASSERT_EQ(ranked.size(), 3u);
  EXPECT_EQ(ranked[0].peer_id, "12D3KooWWin");
  EXPECT_EQ(ranked[1].peer_id, "12D3KooWSeed");
  EXPECT_EQ(ranked[2].peer_id, "12D3KooWLinux");
}

TEST(MeshHopPolicyTest, DirectoryHopPrefersGlobalIpv6Multiaddr) {
  mesh::MeshDirectoryNode node;
  node.peer_id = "12D3KooWDir";
  node.multiaddrs = {"/ip4/10.0.0.9/udp/443/adp/1.0.0/p2p/12D3KooWDir",
                     "/ip6/2001:db8::5/udp/443/adp/1.0.0/p2p/12D3KooWDir"};
  auto hops = CollectDirectoryHopCandidates({node});
  ASSERT_EQ(hops.size(), 1u);
  EXPECT_EQ(hops[0].multiaddr.rfind("/ip6/", 0), 0u);
}

TEST(MeshHopPolicyTest, DhtHopCandidatesUseDhtDiscoveredAffinity) {
  mesh::MeshDirectoryNode node;
  node.peer_id = "12D3KooWDht";
  node.multiaddrs = {"/ip4/8.8.8.8/udp/443/adp/1.0.0/p2p/12D3KooWDht"};
  node.media_relay = true;
  auto dht = CollectDhtHopCandidates({node});
  ASSERT_EQ(dht.size(), 1u);
  EXPECT_EQ(dht[0].affinity, mesh::MeshHopAffinity::DhtDiscovered);
  EXPECT_TRUE(dht[0].advertises_media_relay);
}

TEST(MeshHopPolicyTest, CircuitOrdersDirectoryBeforeDhtBeforeSeed) {
  mesh::MeshDirectoryNode dir;
  dir.peer_id = "12D3KooWDir";
  dir.multiaddrs = {"/ip4/9.9.9.9/udp/443/adp/1.0.0/p2p/12D3KooWDir"};
  mesh::MeshDirectoryNode dht_node;
  dht_node.peer_id = "12D3KooWDht";
  dht_node.multiaddrs = {"/ip4/8.8.8.8/udp/443/adp/1.0.0/p2p/12D3KooWDht"};
  auto hops = BuildCircuitHopList({}, {dir}, {dht_node},
                                  {"/ip4/1.2.3.4/tcp/443/p2p/12D3KooWCmqCKgBL47m25WzUgiAPayf3GqKiRosmPvAqp2MQUFYR"},
                                  false, true);
  ASSERT_EQ(hops.size(), 3u);
  EXPECT_EQ(hops[0].affinity, mesh::MeshHopAffinity::DirectoryNode);
  EXPECT_EQ(hops[1].affinity, mesh::MeshHopAffinity::DhtDiscovered);
  EXPECT_EQ(hops[2].affinity, mesh::MeshHopAffinity::OrgSeed);
}

TEST(MeshHopPolicyTest, CollectLedgerGatewayFiltersCapability) {
  mesh::MeshDirectoryNode plain;
  plain.peer_id = "12D3KooWPlain";
  plain.multiaddrs = {"/ip4/1.1.1.1/udp/443/adp/1.0.0/p2p/12D3KooWPlain"};
  mesh::MeshDirectoryNode gateway;
  gateway.peer_id = "12D3KooWGw";
  gateway.multiaddrs = {"/ip4/2.2.2.2/udp/443/adp/1.0.0/p2p/12D3KooWGw"};
  gateway.ledger_gateway = true;
  auto hops = CollectLedgerGatewayHopCandidates({plain, gateway});
  ASSERT_EQ(hops.size(), 1u);
  EXPECT_EQ(hops[0].peer_id, "12D3KooWGw");
  EXPECT_EQ(hops[0].affinity, mesh::MeshHopAffinity::DirectoryNode);
}

// projects/privacy T4: with trusted relays only, org seeds and Friendly contacts' nodes carry our
// traffic — not plain contacts, directory volunteers or DHT-discovered nodes.
TEST(MeshHopPolicyTest, TrustedRelaysAreOrgSeedsAndFriendlyContacts) {
  const std::string seed = "12D3KooWCmqCKgBL47m25WzUgiAPayf3GqKiRosmPvAqp2MQUFYR";
  Contact friendly = MakeContact("12D3KooWFriend");
  friendly.trust = TrustLevel::Friendly;
  const Contact plain = MakeContact("12D3KooWPlain");
  const auto trusted = TrustedRelayPeerIds({friendly, plain}, {"/ip4/3.208.41.58/udp/443/adp/1.0.0/p2p/" + seed});
  EXPECT_TRUE(trusted.contains(seed));
  EXPECT_TRUE(trusted.contains("12D3KooWFriend"));
  EXPECT_FALSE(trusted.contains("12D3KooWPlain"));

  std::vector<mesh::MeshHopCandidate> hops;
  for (const std::string& id : {std::string("12D3KooWPlain"), seed, std::string("12D3KooWVolunteer"),
                                std::string("12D3KooWFriend")}) {
    mesh::MeshHopCandidate hop;
    hop.peer_id = id;
    hops.push_back(hop);
  }
  const auto kept = KeepTrustedRelays(hops, trusted);
  ASSERT_EQ(kept.size(), 2u);
  EXPECT_EQ(kept[0].peer_id, seed) << "order kept";
  EXPECT_EQ(kept[1].peer_id, "12D3KooWFriend");
}

} // namespace
} // namespace pbr
