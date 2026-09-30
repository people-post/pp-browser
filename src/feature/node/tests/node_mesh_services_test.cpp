#include "feature/node/NodeMeshServices.h"

#include <gtest/gtest.h>
#include "common/PbrCompat.h"

using namespace pbr;

namespace {

MeshConfig AllCapabilities() {
  MeshConfig mesh;
  mesh.capabilities.circuit_relay = true;
  mesh.capabilities.media_relay = true;
  mesh.capabilities.dht = true;
  return mesh;
}

} // namespace

// A client role hosts no node services, whatever its capabilities say.
TEST(NodeMeshServicesTest, ClientHostsNothing) {
  MeshHostConfig cfg;
  ApplyNodeHosting(cfg, AllCapabilities(), /*node_role=*/false);
  EXPECT_FALSE(cfg.host_circuit_relay);
  EXPECT_FALSE(cfg.host_media_relay);
  EXPECT_FALSE(cfg.host_dht);
  EXPECT_FALSE(cfg.host_directory);
}

// A node hosts relays / DHT per capability and always serves the directory twin (N029 nd4).
TEST(NodeMeshServicesTest, NodeHostsPerCapabilityAndDirectory) {
  MeshConfig mesh = AllCapabilities();
  mesh.capabilities.media_relay = false;
  MeshHostConfig cfg;
  ApplyNodeHosting(cfg, mesh, /*node_role=*/true);
  EXPECT_TRUE(cfg.host_circuit_relay);
  EXPECT_FALSE(cfg.host_media_relay);
  EXPECT_TRUE(cfg.host_dht);
  EXPECT_TRUE(cfg.host_directory);

  MeshHostConfig bare;
  ApplyNodeHosting(bare, MeshConfig{}, /*node_role=*/true);
  EXPECT_TRUE(bare.host_directory);
}

// Query peers: directory rows first (N027 L1), then bootstrap seeds, deduped by PeerId.
TEST(NodeMeshServicesTest, QueryPeersDirectoryFirstDeduped) {
  MeshDirectoryNode a;
  a.peer_id = "QmDirA";
  MeshDirectoryNode dup;
  dup.peer_id = "QmSeed1";
  MeshDirectoryNode empty;
  const std::vector<std::string> seeds = {
      "/ip4/1.2.3.4/udp/443/adp/1.0.0/p2p/QmSeed1",
      "/ip4/5.6.7.8/udp/443/adp/1.0.0/p2p/QmSeed2",
      "/ip4/5.6.7.8/udp/443/adp/1.0.0",
  };
  EXPECT_EQ(CollectNodeQueryPeerKeys(seeds, {a, dup, empty}),
            (std::vector<std::string>{"QmDirA", "QmSeed1", "QmSeed2"}));
  EXPECT_EQ(CollectNodeQueryPeerKeys(seeds), (std::vector<std::string>{"QmSeed1", "QmSeed2"}));
}
