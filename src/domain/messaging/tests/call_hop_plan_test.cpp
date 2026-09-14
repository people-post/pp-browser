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

TEST(CallHopPlanTest, SelectLinkPrefersLocal) {
  std::vector<MeshHopCandidate> ranked;
  MeshHopCandidate seed;
  seed.peer_id = "seed";
  seed.multiaddr = "/ip4/1.2.3.4/tcp/443/p2p/seed";
  seed.affinity = MeshHopAffinity::OrgSeed;
  seed.dialable = true;
  ranked.push_back(seed);

  auto out = SelectCallMediaHop(std::move(ranked), CallHopScope::Link, "local", true,
                                "/ip4/10.0.0.1/tcp/1/p2p/local");
  ASSERT_FALSE(out.empty());
  EXPECT_EQ(out.front().peer_id, "local");
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

} // namespace
} // namespace pbr
