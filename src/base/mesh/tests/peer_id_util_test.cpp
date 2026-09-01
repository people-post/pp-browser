#include "base/crypto/MlDsa.h"
#include "base/mesh/PeerIdUtil.h"

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

} // namespace
