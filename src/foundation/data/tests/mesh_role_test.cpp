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
  config.bootstrap_peers = {"/ip4/1.2.3.4/udp/443/adp/2/p2p/12D3KooWConfigOnly"};
  pbr::NormalizeMeshConfig(config);
  ASSERT_EQ(config.bootstrap_peers.size(), 1u);
  EXPECT_EQ(config.bootstrap_peers[0], "/ip4/1.2.3.4/udp/443/adp/2/p2p/12D3KooWConfigOnly");
}

TEST(MeshRoleTest, NormalizeStripsRetiredBootstrapAndFillsDefaults) {
  pbr::MeshConfig config;
  config.bootstrap_peers = {"/ip4/3.208.41.58/udp/443/adp/2/p2p/12D3KooWCmqCKgBL47m25WzUgiAPayf3GqKiRosmPvAqp2MQUFYR"};
  pbr::NormalizeMeshConfig(config);
  ASSERT_EQ(config.bootstrap_peers.size(), pbr::kDefaultMeshBootstrapPeerCount);
  EXPECT_EQ(config.bootstrap_peers[0], pbr::kDefaultMeshBootstrapPeers[0]);
  EXPECT_EQ(config.bootstrap_peers[1], pbr::kDefaultMeshBootstrapPeers[1]);
}

// #416: a config.json that kept the two seeds destroyed in the 2026-10-30 fleet move drops them for
// today's defaults instead of spending ~8 s on each dead door.
TEST(MeshRoleTest, NormalizeDropsTheSeedsOfTheOldFleet) {
  pbr::MeshConfig config;
  config.bootstrap_peers = {"/ip4/54.198.185.139/udp/443/adp/2/p2p/QmbgShE3J3G6fvEHqwTS5ooQiFnYn46rzn6cTexUGWdWaj",
                            "/ip4/44.218.209.223/udp/443/adp/2/p2p/Qmd2m4skBPf7YjPaAKcnhPuhzRv6wEnsf9QheZRANYGuM7"};
  pbr::NormalizeMeshConfig(config);
  ASSERT_EQ(config.bootstrap_peers.size(), pbr::kDefaultMeshBootstrapPeerCount);
  for (const std::string& peer : config.bootstrap_peers) {
    EXPECT_EQ(peer.find("54.198.185.139"), std::string::npos);
    EXPECT_EQ(peer.find("44.218.209.223"), std::string::npos);
  }
}

TEST(MeshRoleTest, ResolveEffectivePrefersDirectoryThenConfig) {
  pbr::MeshConfig config;
  config.bootstrap_peers = {"/ip4/9.9.9.9/udp/443/adp/2/p2p/12D3KooWConfigSeed"};
  pp::mesh::MeshDirectoryNode dir;
  dir.peer_id = "12D3KooWDirNode";
  dir.multiaddrs = {"/ip4/8.8.8.8/udp/443/adp/2/p2p/12D3KooWDirNode"};
  dir.circuit_relay = true;
  const auto peers = pbr::ResolveEffectiveBootstrapPeers(config, {dir});
  ASSERT_EQ(peers.size(), 2u);
  EXPECT_EQ(peers[0], dir.multiaddrs.front());
  EXPECT_EQ(peers[1], config.bootstrap_peers.front());
}

TEST(MeshRoleTest, ResolveEffectiveDialsADirectoryNodeAtItsBestAddress) {
  pbr::MeshConfig config;
  pp::mesh::MeshDirectoryNode dir;
  dir.peer_id = "12D3KooWDirNode";
  dir.multiaddrs = {"/ip4/172.17.0.2/udp/443/adp/2/p2p/12D3KooWDirNode",
                    "/ip4/54.198.185.139/udp/443/adp/2/p2p/12D3KooWDirNode"};
  const auto peers = pbr::ResolveEffectiveBootstrapPeers(config, {dir});
  ASSERT_FALSE(peers.empty());
  EXPECT_EQ(peers[0], dir.multiaddrs[1]) << "the public address, not the first listed";
}

// #379: what a node publishes and what a reader dials leaves out a container's or a LAN's address
// beside a public one; a LAN-only list stays.
TEST(MeshRoleTest, DialWorthyMultiaddrsDropLocalAddressesBesideAPublicOne) {
  const std::string container = "/ip4/172.17.0.2/udp/443/adp/2/p2p/12D3KooWNode";
  const std::string loopback = "/ip4/127.0.0.1/udp/443/adp/2/p2p/12D3KooWNode";
  const std::string ula = "/ip6/fd12::5/udp/443/adp/2/p2p/12D3KooWNode";
  const std::string pub = "/ip4/54.198.185.139/udp/443/adp/2/p2p/12D3KooWNode";
  const std::string named = "/dns4/relay.example/udp/443/adp/2/p2p/12D3KooWNode";
  EXPECT_EQ(pbr::DialWorthyMultiaddrs({container, loopback, pub, ula, named}), (std::vector<std::string>{pub, named}));
  EXPECT_EQ(pbr::DialWorthyMultiaddrs({container, ula}), (std::vector<std::string>{container, ula}));
}

TEST(MeshRoleTest, ResolveEffectiveFallsBackToHardcoded) {
  pbr::MeshConfig config;
  config.bootstrap_peers.clear();
  const auto peers = pbr::ResolveEffectiveBootstrapPeers(config, {});
  ASSERT_EQ(peers.size(), pbr::kDefaultMeshBootstrapPeerCount);
  EXPECT_EQ(peers[0], pbr::kDefaultMeshBootstrapPeers[0]);
}

TEST(MeshRoleTest, PeerIdFromMultiaddr) {
  EXPECT_EQ(pbr::PeerIdFromMultiaddr(pbr::kDefaultMeshBootstrapPeer), "QmZyhSEd95N31Az5Fvr9ZJ9Ugarzy5B1pbFfuUXtxEC9Cv");
  EXPECT_TRUE(pbr::PeerIdFromMultiaddr("/ip4/1.2.3.4/udp/443/adp/2").empty());
}
