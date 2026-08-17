#include "base/p2p/CallMediaFrameCrypto.h"
#include "base/p2p/LanMdnsDiscovery.h"
#include "base/p2p/PeerSessionManager.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(LanMdnsDiscoveryTest, EncodeDecodeDnsName) {
  auto encoded = LanMdnsDiscovery::EncodeDnsName("_pp-browser._tcp.local");
  ASSERT_TRUE(encoded);
  size_t next = 0;
  auto decoded = LanMdnsDiscovery::DecodeDnsName(*encoded, 0, &next);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(*decoded, "_pp-browser._tcp.local");
  EXPECT_EQ(next, encoded->size());
}

TEST(LanMdnsDiscoveryTest, BuildMultiaddrFromDiscoveredPeer) {
  LanMdnsDiscoveredPeer peer;
  peer.peer_id_base58 = "QmTestPeer";
  peer.host_ip = "192.168.1.42";
  peer.tcp_port = 18517;
  auto ma = LanMdnsDiscovery::BuildMultiaddr(peer);
  ASSERT_TRUE(ma);
  EXPECT_EQ(*ma, "/ip4/192.168.1.42/tcp/18517/p2p/QmTestPeer");
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

TEST(PeerSessionManagerCircuitTest, CircuitHopKeyIncludesProtocol) {
  EXPECT_EQ(PeerSessionManager::CircuitHopKey("QmA", "/pp-browser/call-media/1.0.0"),
            "QmA\x1f/pp-browser/call-media/1.0.0");
  EXPECT_NE(PeerSessionManager::CircuitHopKey("QmA", "/pp-browser/media-relay/1.0.0"),
            PeerSessionManager::CircuitHopKey("QmA", "/pp-browser/call-media/1.0.0"));
}

} // namespace
} // namespace pbr
