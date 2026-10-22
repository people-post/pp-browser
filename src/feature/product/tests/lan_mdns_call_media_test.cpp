#include "mesh/l4/circuit/client/AmpCircuitHopRegistry.h"
#include "feature/call_media/CallMediaFrameCrypto.h"
#include "mesh/reachability/LanMdnsDiscovery.h"
#include "common/MeshAlias.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(LanMdnsDiscoveryTest, EncodeDecodeDnsName) {
  auto encoded = mesh::LanMdnsDiscovery::EncodeDnsName("_pp-mesh._tcp.local");
  ASSERT_TRUE(encoded);
  size_t next = 0;
  auto decoded = mesh::LanMdnsDiscovery::DecodeDnsName(*encoded, 0, &next);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(*decoded, "_pp-mesh._tcp.local");
  EXPECT_EQ(next, encoded->size());
}

TEST(LanMdnsDiscoveryTest, BuildMultiaddrFromDiscoveredPeer) {
  mesh::LanMdnsDiscoveredPeer peer;
  peer.peer_id_base58 = "QmTestPeer";
  peer.host_ip = "192.168.1.42";
  peer.tcp_port = 18517;
  auto ma = mesh::LanMdnsDiscovery::BuildMultiaddr(peer);
  ASSERT_TRUE(ma);
  EXPECT_EQ(*ma, "/ip4/192.168.1.42/tcp/18517/p2p/QmTestPeer");
}

TEST(LanMdnsDiscoveryTest, BuildAdpMultiaddrFromDiscoveredPeer) {
  mesh::LanMdnsDiscoveredPeer peer;
  peer.peer_id_base58 = "QmTestPeer";
  peer.host_ip = "192.168.1.42";
  peer.tcp_port = 18517;
  peer.amp_udp_port = 19001;
  auto ma = mesh::LanMdnsDiscovery::BuildAdpMultiaddr(peer);
  ASSERT_TRUE(ma);
  EXPECT_EQ(*ma, "/ip4/192.168.1.42/udp/19001/adp/1.0.0/p2p/QmTestPeer");
  peer.amp_udp_port = 0;
  EXPECT_FALSE(mesh::LanMdnsDiscovery::BuildAdpMultiaddr(peer).has_value());
}

TEST(CallMediaFrameCryptoTest, RoundTripAudioFrame) {
  ByteVector key(32, 0x11);
  const std::string call_id = "call-abc";
  constexpr uint32_t epoch = 1;
  constexpr uint32_t seq = 7;
  std::vector<uint8_t> opus = {0x01, 0x02, 0x03, 0x04};
  auto encrypted = EncryptCallMediaAudioFrame(key, call_id, epoch, seq, 0, opus);
  ASSERT_TRUE(encrypted);
  auto decrypted = DecryptCallMediaAudioFrame(key, call_id, epoch, *encrypted);
  ASSERT_TRUE(decrypted);
  EXPECT_EQ(*decrypted, opus);
}

TEST(AmpCircuitHopRegistryTest, KeyIncludesProtocol) {
  EXPECT_EQ(mesh::AmpCircuitHopRegistry::Key("QmA", "/pp/realtime/1.0.0"),
            "QmA\x1f/pp/realtime/1.0.0");
  EXPECT_NE(mesh::AmpCircuitHopRegistry::Key("QmA", "/pp-mesh/datagram-relay/1.0.0"),
            mesh::AmpCircuitHopRegistry::Key("QmA", "/pp/realtime/1.0.0"));
}

} // namespace
} // namespace pbr
