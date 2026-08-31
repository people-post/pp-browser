#include "base/data/Libp2pRole.h"

#include <gtest/gtest.h>

TEST(Libp2pRoleTest, NormalizeFillsBootstrapPeers) {
  pbr::Libp2pConfig config;
  config.bootstrap_peers.clear();
  pbr::NormalizeLibp2pConfig(config);
  ASSERT_EQ(config.bootstrap_peers.size(), 1u);
  EXPECT_EQ(config.bootstrap_peers[0], pbr::kDefaultLibp2pBootstrapPeer);
}

TEST(Libp2pRoleTest, PeerIdFromMultiaddr) {
  EXPECT_EQ(pbr::PeerIdFromMultiaddr(pbr::kDefaultLibp2pBootstrapPeer),
            "12D3KooWCmqCKgBL47m25WzUgiAPayf3GqKiRosmPvAqp2MQUFYR");
  EXPECT_TRUE(pbr::PeerIdFromMultiaddr("/ip4/1.2.3.4/udp/443/adp/1.0.0").empty());
}
