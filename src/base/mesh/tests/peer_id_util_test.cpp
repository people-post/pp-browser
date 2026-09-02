#include "foundation/crypto/MlDsa.h"
#include "base/mesh/identity/PeerId.h"
#include "base/mesh/identity/PeerIdUtil.h"

#include <gtest/gtest.h>

#include <vector>

namespace {

using namespace pbr;

TEST(PeerIdUtilTest, RejectsWrongLength) {
  const std::vector<uint8_t> short_key(32, 0x01);
  EXPECT_FALSE(static_cast<bool>(PeerIdFromMlDsaPublicKey(short_key)));
}

TEST(PeerIdUtilTest, DerivesPeerIdFromGeneratedKey) {
  auto keys = MlDsa::GenerateKeyPair();
  ASSERT_TRUE(static_cast<bool>(keys)) << keys.error().message;
  auto peer_id = PeerIdFromMlDsaPublicKey(keys->public_key);
  ASSERT_TRUE(static_cast<bool>(peer_id)) << peer_id.error().message;
  EXPECT_FALSE(peer_id->empty());
  // Large ML-DSA keys use sha2-256 PeerIds (Qm…); identity multihashes are 12D3KooW….
  EXPECT_TRUE(peer_id->rfind("Qm", 0) == 0u || peer_id->rfind("12D3KooW", 0) == 0u)
      << *peer_id;
}

TEST(PeerIdUtilTest, SamePubkeySamePeerId) {
  auto keys = MlDsa::GenerateKeyPair();
  ASSERT_TRUE(static_cast<bool>(keys));
  auto a = PeerIdFromMlDsaPublicKey(keys->public_key);
  auto b = PeerIdFromMlDsaPublicKey(keys->public_key);
  ASSERT_TRUE(static_cast<bool>(a));
  ASSERT_TRUE(static_cast<bool>(b));
  EXPECT_EQ(*a, *b);
}

TEST(PeerIdUtilTest, GoldenMlDsa65Vectors) {
  const std::vector<uint8_t> key42(kMlDsa65PublicKeyBytes, 0x42);
  auto id42 = PeerIdFromMlDsaPublicKey(key42);
  ASSERT_TRUE(static_cast<bool>(id42)) << id42.error().message;
  EXPECT_EQ(*id42, "QmTWkSQAcGsETogTFKrPJ3GzdHG3vP9UwXVyp2HeK9YBvP");
  EXPECT_EQ(id42->rfind("Qm", 0), 0u);

  const std::vector<uint8_t> key01(kMlDsa65PublicKeyBytes, 0x01);
  auto id01 = PeerIdFromMlDsaPublicKey(key01);
  ASSERT_TRUE(static_cast<bool>(id01)) << id01.error().message;
  EXPECT_EQ(*id01, "QmeKYz9h9AozszFqAkRzwxx4xGheEMYRVrLHuh7EMG7Bie");
  EXPECT_EQ(id01->rfind("Qm", 0), 0u);
}

TEST(PeerIdTest, InlineProtobufKeyUsesIdentityPrefix) {
  // Ed25519-sized protobuf public key wire (type=1, 32 zero bytes) — inline identity multihash.
  std::vector<uint8_t> wire{0x08, 0x01, 0x12, 0x20};
  wire.insert(wire.end(), 32, 0x00);
  ASSERT_LE(wire.size(), PeerId::kMaxInlineKeyLength);

  const auto peer_id = PeerId::FromProtobufPublicKey(wire).ToBase58();
  EXPECT_EQ(peer_id.rfind("12D3KooW", 0), 0u) << peer_id;
}

TEST(PeerIdTest, LargeProtobufKeyUsesSha256Prefix) {
  std::vector<uint8_t> wire(PeerId::kMaxInlineKeyLength + 1, 0xAB);
  const auto peer_id = PeerId::FromProtobufPublicKey(wire).ToBase58();
  EXPECT_EQ(peer_id.rfind("Qm", 0), 0u) << peer_id;
}

} // namespace
