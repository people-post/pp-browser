#include "common/MeshConfig.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {
namespace {

// N001: a phone is always a mesh client; a desktop is a node when node_enabled.
TEST(MeshConfigTest, APhoneIsAlwaysAClient) {
  MeshConfig config;
  config.node_enabled = true;
  EXPECT_EQ(MeshRoleOf(EffectiveMeshConfig(config, /*mobile=*/true)), MeshRole::Client);
  EXPECT_FALSE(EffectiveMeshConfig(config, true).node_enabled) << "readers of the effective config see a client";
  EXPECT_EQ(MeshRoleOf(EffectiveMeshConfig(config, /*mobile=*/false)), MeshRole::Node);
  config.node_enabled = false;
  EXPECT_EQ(MeshRoleOf(EffectiveMeshConfig(config, false)), MeshRole::Client);
}

// N027: the directory's mesh nodes first, then the config's seeds, one address per PeerId.
TEST(MeshConfigTest, SeedsAreTheDirectorysNodesThenTheConfigs) {
  MeshConfig config;
  config.bootstrap_peers = {"/ip4/203.0.113.1/udp/443/adp/2/p2p/SeedA", "/ip4/203.0.113.9/udp/443/adp/2/p2p/NodeB"};
  mesh::MeshDirectoryNode node;
  node.peer_id = "NodeB";
  node.multiaddrs = {"/ip4/198.51.100.2/udp/443/adp/2/p2p/NodeB"};
  EXPECT_EQ(EffectiveBootstrapPeers(config, {node}),
            (std::vector<std::string>{"/ip4/198.51.100.2/udp/443/adp/2/p2p/NodeB",
                                      "/ip4/203.0.113.1/udp/443/adp/2/p2p/SeedA"}));
  EXPECT_EQ(EffectiveBootstrapPeers(config), config.bootstrap_peers) << "no directory: the config's, in order";
}

} // namespace
} // namespace pbr
