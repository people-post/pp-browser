#include "base/data/Libp2pRole.h"

#include "base/platform/Platform.h"

#include <gtest/gtest.h>

TEST(Libp2pRoleTest, NormalizeFillsDefaults) {
  pbr::Libp2pConfig config;
  config.listen_multiaddr.clear();
  config.bootstrap_peers.clear();
  pbr::NormalizeLibp2pConfig(config);
  EXPECT_EQ(config.listen_multiaddr, pbr::kPreferredLibp2pListenMultiaddr);
  ASSERT_EQ(config.bootstrap_peers.size(), 1u);
  EXPECT_EQ(config.bootstrap_peers[0], pbr::kDefaultLibp2pBootstrapPeer);
}

TEST(Libp2pRoleTest, TcpPortHelpers) {
  EXPECT_EQ(pbr::TcpPortFromMultiaddr("/ip4/0.0.0.0/tcp/18517"), 18517);
  EXPECT_EQ(pbr::ReplaceTcpPortInMultiaddr("/ip4/0.0.0.0/tcp/18517", 18520), "/ip4/0.0.0.0/tcp/18520");
  EXPECT_EQ(pbr::PeerIdFromMultiaddr(pbr::kDefaultLibp2pBootstrapPeer),
            "12D3KooWCmqCKgBL47m25WzUgiAPayf3GqKiRosmPvAqp2MQUFYR");
}

TEST(Libp2pRoleTest, BuildListenCandidatesPreferredRange) {
  const auto candidates = pbr::BuildLibp2pListenCandidates("/ip4/0.0.0.0/tcp/18517");
  ASSERT_GE(candidates.size(), 11u);
  EXPECT_EQ(candidates.front(), "/ip4/0.0.0.0/tcp/18517");
  EXPECT_EQ(candidates.back(), "/ip4/0.0.0.0/tcp/0");
  bool saw_18518 = false;
  bool saw_18526 = false;
  for (const std::string& c : candidates) {
    if (c == "/ip4/0.0.0.0/tcp/18518") {
      saw_18518 = true;
    }
    if (c == "/ip4/0.0.0.0/tcp/18526") {
      saw_18526 = true;
    }
  }
  EXPECT_TRUE(saw_18518);
  EXPECT_TRUE(saw_18526);
}

TEST(Libp2pRoleTest, ResolveRoleHonorsNodeEnabledOnDesktop) {
  pbr::Libp2pConfig on;
  on.node_enabled = true;
  pbr::Libp2pConfig off;
  off.node_enabled = false;
  // Desktop CI / Linux host: Node when enabled, Client when disabled.
  // Mobile builds always resolve Client (covered by Platform::IsMobile).
  if (!pbr::Platform::IsMobile()) {
    EXPECT_EQ(pbr::ResolveLibp2pRole(on), pbr::Libp2pRole::Node);
    EXPECT_EQ(pbr::ResolveLibp2pRole(off), pbr::Libp2pRole::Client);
  } else {
    EXPECT_EQ(pbr::ResolveLibp2pRole(on), pbr::Libp2pRole::Client);
    EXPECT_EQ(pbr::ResolveLibp2pRole(off), pbr::Libp2pRole::Client);
  }
}
