#include "domain/messaging/CallHopPlan.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(CallHopPlanTest, EmptyRemotesIsWide) {
  EXPECT_EQ(InferCallHopScope({"/ip4/10.0.0.1/tcp/1/p2p/local"}, {}), CallHopScope::Wide);
}

TEST(CallHopPlanTest, MissingRemoteMasIsWide) {
  std::unordered_map<std::string, std::vector<std::string>> remotes;
  remotes["account:B"] = {};
  EXPECT_EQ(InferCallHopScope({"/ip4/10.0.0.1/tcp/1/p2p/local"}, remotes), CallHopScope::Wide);
}

TEST(CallHopPlanTest, SameSlash24IsLink) {
  std::unordered_map<std::string, std::vector<std::string>> remotes;
  remotes["account:B"] = {"/ip4/10.0.0.2/tcp/1/p2p/b"};
  remotes["account:C"] = {"/ip4/10.0.0.3/udp/4001/adp/1.0.0/p2p/c"};
  EXPECT_EQ(InferCallHopScope({"/ip4/10.0.0.1/tcp/18517/p2p/local"}, remotes), CallHopScope::Link);
}

TEST(CallHopPlanTest, MixedPrivatePublicIsWide) {
  std::unordered_map<std::string, std::vector<std::string>> remotes;
  remotes["account:B"] = {"/ip4/10.0.0.2/tcp/1/p2p/b"};
  remotes["account:C"] = {"/ip4/54.1.2.3/tcp/443/p2p/c"};
  EXPECT_EQ(InferCallHopScope({"/ip4/10.0.0.1/tcp/1/p2p/local"}, remotes), CallHopScope::Wide);
}

TEST(CallHopPlanTest, AllPrivateDifferentSubnetIsSite) {
  std::unordered_map<std::string, std::vector<std::string>> remotes;
  remotes["account:B"] = {"/ip4/192.168.1.2/tcp/1/p2p/b"};
  EXPECT_EQ(InferCallHopScope({"/ip4/10.0.0.1/tcp/1/p2p/local"}, remotes), CallHopScope::Site);
}

TEST(CallHopPlanTest, SelectLinkPrefersLocalOnlyWhenLanConfirmed) {
  std::vector<MeshHopCandidate> ranked;
  MeshHopCandidate seed;
  seed.peer_id = "seed";
  seed.multiaddr = "/ip4/1.2.3.4/tcp/443/p2p/seed";
  seed.affinity = MeshHopAffinity::OrgSeed;
  seed.dialable = true;
  ranked.push_back(seed);

  auto no_lan = SelectCallMediaHop(ranked, CallHopScope::Link, "local", true,
                                   "/ip4/10.0.0.1/tcp/1/p2p/local", false);
  ASSERT_FALSE(no_lan.empty());
  EXPECT_EQ(no_lan.front().peer_id, "seed");

  auto with_lan = SelectCallMediaHop(std::move(ranked), CallHopScope::Link, "local", true,
                                     "/ip4/10.0.0.1/tcp/1/p2p/local", true);
  ASSERT_FALSE(with_lan.empty());
  EXPECT_EQ(with_lan.front().peer_id, "local");
}

TEST(CallHopPlanTest, SelectSiteNeverPreferLocal) {
  EXPECT_FALSE(PreferLocalAllowedForScope(CallHopScope::Site, true, "/ip4/10.0.0.1/tcp/1/p2p/local",
                                          true));
  std::vector<MeshHopCandidate> ranked;
  MeshHopCandidate seed;
  seed.peer_id = "seed";
  seed.multiaddr = "/ip4/54.1.2.3/tcp/443/p2p/seed";
  seed.affinity = MeshHopAffinity::OrgSeed;
  seed.dialable = true;
  ranked.push_back(seed);
  auto out = SelectCallMediaHop(std::move(ranked), CallHopScope::Site, "local", true,
                                "/ip4/10.0.0.1/tcp/1/p2p/local", true);
  ASSERT_FALSE(out.empty());
  EXPECT_EQ(out.front().peer_id, "seed");
}

TEST(CallHopPlanTest, SelectWidePromotesPublicSeedNotPrivateLocal) {
  std::vector<MeshHopCandidate> ranked;
  MeshHopCandidate contact;
  contact.peer_id = "contact";
  contact.multiaddr = "/ip4/10.0.0.9/tcp/1/p2p/contact";
  contact.affinity = MeshHopAffinity::Contact;
  contact.dialable = true;
  ranked.push_back(contact);
  MeshHopCandidate seed;
  seed.peer_id = "seed";
  seed.multiaddr = "/ip4/54.1.2.3/tcp/443/p2p/seed";
  seed.affinity = MeshHopAffinity::OrgSeed;
  seed.dialable = true;
  ranked.push_back(seed);

  auto out = SelectCallMediaHop(std::move(ranked), CallHopScope::Wide, "local", true,
                                "/ip4/10.0.0.1/tcp/1/p2p/local");
  ASSERT_FALSE(out.empty());
  EXPECT_EQ(out.front().peer_id, "seed");
  for (const auto& hop : out) {
    EXPECT_NE(hop.peer_id, "local");
  }
}

TEST(CallHopPlanTest, SelectWideAllowsPublicPreferLocal) {
  std::vector<MeshHopCandidate> ranked;
  MeshHopCandidate seed;
  seed.peer_id = "seed";
  seed.multiaddr = "/ip4/1.2.3.4/tcp/443/p2p/seed";
  seed.affinity = MeshHopAffinity::OrgSeed;
  seed.dialable = true;
  ranked.push_back(seed);

  auto out = SelectCallMediaHop(std::move(ranked), CallHopScope::Wide, "local", true,
                                "/ip4/54.9.8.7/tcp/443/p2p/local");
  ASSERT_FALSE(out.empty());
  EXPECT_EQ(out.front().peer_id, "local");
  EXPECT_FALSE(PreferLocalAllowedForScope(CallHopScope::Wide, true,
                                          "/ip4/10.0.0.1/tcp/1/p2p/local"));
  EXPECT_TRUE(PreferLocalAllowedForScope(CallHopScope::Wide, true,
                                         "/ip4/54.9.8.7/tcp/443/p2p/local"));
}

TEST(CallHopPlanTest, GuestMayDialPrivateHopRejectsWhenLocalHasPublic) {
  EXPECT_FALSE(GuestMayDialPrivateHopMa(
      "/ip4/192.168.1.132/udp/1/p2p/hop",
      {"/ip4/10.0.0.1/tcp/1/p2p/local", "/ip4/54.1.2.3/tcp/443/p2p/local"}));
  EXPECT_TRUE(GuestMayDialPrivateHopMa(
      "/ip4/10.0.0.5/tcp/1/p2p/hop", {"/ip4/10.0.0.1/tcp/1/p2p/local"}));
  EXPECT_FALSE(GuestMayDialPrivateHopMa(
      "/ip4/192.168.1.132/udp/1/p2p/hop", {"/ip4/10.0.0.1/tcp/1/p2p/local"}));
}


TEST(CallHopPlanTest, LocalAdvertiseHasPublicTreatsGlobalIpv6) {
  EXPECT_TRUE(LocalAdvertiseHasPublicIpv4(
      {"/ip6/2001:db8::1/udp/19001/adp/1.0.0/p2p/local"}));
  EXPECT_FALSE(LocalAdvertiseHasPublicIpv4(
      {"/ip6/fe80::1/udp/19001/adp/1.0.0/p2p/local"}));
  EXPECT_FALSE(GuestMayDialPrivateHopMa(
      "/ip4/192.168.1.132/udp/1/p2p/hop",
      {"/ip6/2001:db8::1/udp/19001/adp/1.0.0/p2p/local"}));
  EXPECT_TRUE(PreferLocalAllowedForScope(CallHopScope::Wide, true,
                                         "/ip6/2001:db8::1/udp/19001/adp/1.0.0/p2p/local"));
  // Link-local / loopback /ip6 must not count as publicly dialable PreferLocal.
  EXPECT_FALSE(PreferLocalAllowedForScope(CallHopScope::Wide, true,
                                          "/ip6/fe80::1/udp/19001/adp/1.0.0/p2p/local"));
  EXPECT_FALSE(PreferLocalAllowedForScope(CallHopScope::Wide, true,
                                          "/ip6/::1/udp/19001/adp/1.0.0/p2p/local"));
  EXPECT_FALSE(PreferLocalAllowedForScope(CallHopScope::Link, true,
                                          "/ip6/fe80::1/udp/19001/adp/1.0.0/p2p/local", true));
}

// --- V050 gt4: the hop a group forms on at the third join ---------------------------------------

CallHopReport Report(std::optional<bool> planned_ok, std::vector<std::string> reachable,
                     std::vector<std::string> unreachable = {}) {
  CallHopReport r;
  r.planned_hop_ok = planned_ok;
  r.reachable_hops = std::move(reachable);
  r.unreachable_hops = std::move(unreachable);
  return r;
}

TEST(GroupHopAtJoinTest, KeepsThePlannedHopUnlessSomeoneCannotReachIt) {
  GroupHopJoinInput in;
  in.planned_hop = "P";
  in.ranked_hops = {"X", "P", "Y"};
  in.reports["b"] = Report(true, {"P", "X"});
  in.reports["c"] = Report(std::nullopt, {});  // old peer / probe not finished
  const auto out = DecideGroupHopAtJoin(in);
  EXPECT_EQ(out.action, GroupHopAtJoin::UsePlanned);
  EXPECT_EQ(out.hop, "P");
}

TEST(GroupHopAtJoinTest, OneAdjustmentToTheFirstRankedHopEveryReporterReached) {
  GroupHopJoinInput in;
  in.planned_hop = "P";
  in.ranked_hops = {"P", "X", "Y"};
  in.reports["b"] = Report(true, {"P", "Y", "X"});
  in.reports["c"] = Report(false, {"Y"});  // cannot reach P; reached only Y
  const auto out = DecideGroupHopAtJoin(in);
  EXPECT_EQ(out.action, GroupHopAtJoin::UseAlternative);
  EXPECT_EQ(out.hop, "Y") << "X is ranked higher but C never reached it";
}

TEST(GroupHopAtJoinTest, ReportersWithoutAListDoNotConstrainTheAlternative) {
  GroupHopJoinInput in;
  in.planned_hop = "P";
  in.ranked_hops = {"X"};
  in.reports["b"] = Report(std::nullopt, {});
  in.reports["c"] = Report(false, {"X"});
  const auto out = DecideGroupHopAtJoin(in);
  EXPECT_EQ(out.action, GroupHopAtJoin::UseAlternative);
  EXPECT_EQ(out.hop, "X");
}

TEST(GroupHopAtJoinTest, NoSharedHopRefusesTheJoiner) {
  GroupHopJoinInput in;
  in.planned_hop = "P";
  in.ranked_hops = {"P", "X"};
  in.reports["b"] = Report(true, {"P"}, {"X"});  // b tried X and failed
  in.reports["c"] = Report(false, {"X"}, {"P"});
  EXPECT_EQ(DecideGroupHopAtJoin(in).action, GroupHopAtJoin::RefuseJoiner);

  in.reports["b"] = Report(true, {"P"});
  in.reports["c"] = Report(false, {}, {"P"});  // c reached nothing: no evidence for any alternative
  EXPECT_EQ(DecideGroupHopAtJoin(in).action, GroupHopAtJoin::RefuseJoiner);
}

// An invitee may accept before its probes finish: a hop it has not reported either way is
// unknown and must not rule the alternative out (hard lab: B accepted at once, C 8 s later).
TEST(GroupHopAtJoinTest, AHopStillBeingProbedExcludesNothing) {
  GroupHopJoinInput in;
  in.planned_hop = "P";
  in.ranked_hops = {"P", "X"};
  in.reports["b"] = Report(true, {"P"});           // X still probing when b accepted
  in.reports["c"] = Report(false, {"X"}, {"P"});  // c cannot reach P, reached X
  const auto out = DecideGroupHopAtJoin(in);
  EXPECT_EQ(out.action, GroupHopAtJoin::UseAlternative);
  EXPECT_EQ(out.hop, "X");
}

TEST(GroupHopAtJoinTest, NoPlanMeansTheUsualPick) {
  GroupHopJoinInput in;
  in.reports["c"] = Report(false, {});
  const auto out = DecideGroupHopAtJoin(in);
  EXPECT_EQ(out.action, GroupHopAtJoin::UsePlanned);
  EXPECT_TRUE(out.hop.empty());
}

} // namespace
} // namespace pbr
