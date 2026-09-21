#include "foundation/data/MeshRole.h"

#include <gtest/gtest.h>

TEST(MeshRoleTest, NormalizeFillsBootstrapPeers) {
  pbr::MeshConfig config;
  config.bootstrap_peers.clear();
  pbr::NormalizeMeshConfig(config);
  ASSERT_EQ(config.bootstrap_peers.size(), pbr::kDefaultMeshBootstrapPeerCount);
  EXPECT_EQ(config.bootstrap_peers[0], pbr::kDefaultMeshBootstrapPeers[0]);
  EXPECT_EQ(config.bootstrap_peers[1], pbr::kDefaultMeshBootstrapPeers[1]);
  EXPECT_EQ(config.bootstrap_peers[0], pbr::kDefaultMeshBootstrapPeer);
}

TEST(MeshRoleTest, NormalizeKeepsConfigBootstrapPeers) {
  pbr::MeshConfig config;
  config.bootstrap_peers = {"/ip4/1.2.3.4/udp/443/adp/1.0.0/p2p/12D3KooWConfigOnly"};
  pbr::NormalizeMeshConfig(config);
  ASSERT_EQ(config.bootstrap_peers.size(), 1u);
  EXPECT_EQ(config.bootstrap_peers[0], "/ip4/1.2.3.4/udp/443/adp/1.0.0/p2p/12D3KooWConfigOnly");
}

TEST(MeshRoleTest, NormalizeStripsRetiredBootstrapAndFillsDefaults) {
  pbr::MeshConfig config;
  config.bootstrap_peers = {
      "/ip4/3.208.41.58/udp/443/adp/1.0.0/p2p/12D3KooWCmqCKgBL47m25WzUgiAPayf3GqKiRosmPvAqp2MQUFYR"};
  pbr::NormalizeMeshConfig(config);
  ASSERT_EQ(config.bootstrap_peers.size(), pbr::kDefaultMeshBootstrapPeerCount);
  EXPECT_EQ(config.bootstrap_peers[0], pbr::kDefaultMeshBootstrapPeers[0]);
  EXPECT_EQ(config.bootstrap_peers[1], pbr::kDefaultMeshBootstrapPeers[1]);
}

TEST(MeshRoleTest, ResolveEffectivePrefersDirectoryThenConfig) {
  pbr::MeshConfig config;
  config.bootstrap_peers = {"/ip4/9.9.9.9/udp/443/adp/1.0.0/p2p/12D3KooWConfigSeed"};
  pbr::MeshDirectoryNode dir;
  dir.peer_id = "12D3KooWDirNode";
  dir.multiaddrs = {"/ip4/8.8.8.8/udp/443/adp/1.0.0/p2p/12D3KooWDirNode"};
  dir.circuit_relay = true;
  const auto peers = pbr::ResolveEffectiveBootstrapPeers(config, {dir});
  ASSERT_EQ(peers.size(), 2u);
  EXPECT_EQ(peers[0], dir.multiaddrs.front());
  EXPECT_EQ(peers[1], config.bootstrap_peers.front());
}

TEST(MeshRoleTest, ResolveEffectiveFallsBackToHardcoded) {
  pbr::MeshConfig config;
  config.bootstrap_peers.clear();
  const auto peers = pbr::ResolveEffectiveBootstrapPeers(config, {});
  ASSERT_EQ(peers.size(), pbr::kDefaultMeshBootstrapPeerCount);
  EXPECT_EQ(peers[0], pbr::kDefaultMeshBootstrapPeers[0]);
}

TEST(MeshRoleTest, PeerIdFromMultiaddr) {
  EXPECT_EQ(pbr::PeerIdFromMultiaddr(pbr::kDefaultMeshBootstrapPeer),
            "QmbgShE3J3G6fvEHqwTS5ooQiFnYn46rzn6cTexUGWdWaj");
  EXPECT_TRUE(pbr::PeerIdFromMultiaddr("/ip4/1.2.3.4/udp/443/adp/1.0.0").empty());
}
