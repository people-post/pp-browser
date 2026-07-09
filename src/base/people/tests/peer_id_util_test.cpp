#include "base/people/Ed25519Signer.h"
#include "libp2p/integration/host/PeerIdUtil.h"

#include <gtest/gtest.h>

#include <array>
#include <vector>

namespace {

using namespace pbr;

// Fixed 32-byte Ed25519 public key (not a live keypair — PeerId derivation only).
constexpr std::array<uint8_t, 32> kFixedEd25519Pubkey = {
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00,
    0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90, 0xa0, 0xb0, 0xc0, 0xd0, 0xe0, 0xf0, 0x01,
};

// Stable vector: PeerIdFromEd25519PublicKey(kFixedEd25519Pubkey).
constexpr const char* kExpectedPeerIdBase58 = "12D3KooWAyFM454cpC52KuBDk2nHhEd67gAuwzkPNHC8V75uUndE";

TEST(PeerIdUtilTest, RejectsWrongLength) {
  const std::vector<uint8_t> short_key(16, 0x01);
  EXPECT_FALSE(static_cast<bool>(PeerIdFromEd25519PublicKey(short_key)));
}

TEST(PeerIdUtilTest, DerivesStableBase58PeerId) {
  const std::vector<uint8_t> pubkey(kFixedEd25519Pubkey.begin(), kFixedEd25519Pubkey.end());
  auto peer_id = PeerIdFromEd25519PublicKey(pubkey);
  ASSERT_TRUE(static_cast<bool>(peer_id)) << peer_id.error().message;
  EXPECT_FALSE(peer_id->empty());
  // Capture actual value on first failure so the constant can be updated if needed.
  EXPECT_EQ(*peer_id, kExpectedPeerIdBase58) << "actual peer_id=" << *peer_id;
}

TEST(PeerIdUtilTest, MatchesGeneratedKeypairPubkey) {
  auto keys = Ed25519Signer::GenerateKeyPair();
  ASSERT_TRUE(static_cast<bool>(keys));
  auto peer_id = PeerIdFromEd25519PublicKey(keys->public_key);
  ASSERT_TRUE(static_cast<bool>(peer_id)) << peer_id.error().message;
  EXPECT_FALSE(peer_id->empty());
  // Ed25519 inlined PeerIds are identity multihashes → base58 typically "12D3KooW…".
  EXPECT_EQ(peer_id->rfind("12D3KooW", 0), 0u) << *peer_id;
}

} // namespace
