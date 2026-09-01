#include "base/data/MeshRole.h"

#include <gtest/gtest.h>

TEST(MeshRoleTest, NormalizeFillsBootstrapPeers) {
  pbr::MeshConfig config;
  config.bootstrap_peers.clear();
  pbr::NormalizeMeshConfig(config);
  ASSERT_EQ(config.bootstrap_peers.size(), 1u);
  EXPECT_EQ(config.bootstrap_peers[0], pbr::kDefaultMeshBootstrapPeer);
}

TEST(MeshRoleTest, PeerIdFromMultiaddr) {
  EXPECT_EQ(pbr::PeerIdFromMultiaddr(pbr::kDefaultMeshBootstrapPeer),
            "12D3KooWCmqCKgBL47m25WzUgiAPayf3GqKiRosmPvAqp2MQUFYR");
  EXPECT_TRUE(pbr::PeerIdFromMultiaddr("/ip4/1.2.3.4/udp/443/adp/1.0.0").empty());
}
