#include "foundation/crypto/MlDsa.h"
#include "foundation/crypto/PeerIdForm.h"

#include "crypto/PeerId.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

// MR007-CARD: a card's relay names a PeerId of an ML-DSA key's form (MESH_IDENTITY's two fixtures and a real key).
TEST(PeerIdFormTest, EveryMlDsaPeerIdHasTheForm) {
  EXPECT_TRUE(IsMlDsaPeerIdForm("QmTWkSQAcGsETogTFKrPJ3GzdHG3vP9UwXVyp2HeK9YBvP"));
  EXPECT_TRUE(IsMlDsaPeerIdForm("QmeKYz9h9AozszFqAkRzwxx4xGheEMYRVrLHuh7EMG7Bie"));
  auto keys = MlDsa::GenerateKeyPair();
  ASSERT_TRUE(keys);
  auto peer_id = pp::PeerIdFromMlDsaPublicKey(keys->public_key);
  ASSERT_TRUE(peer_id);
  EXPECT_TRUE(IsMlDsaPeerIdForm(*peer_id));
}

TEST(PeerIdFormTest, AnythingElseIsRefused) {
  EXPECT_FALSE(IsMlDsaPeerIdForm(""));
  EXPECT_FALSE(IsMlDsaPeerIdForm("QmTWkSQAcGsETogTFKrPJ3GzdHG3vP9UwXVyp2HeK9YBv")) << "45 characters";
  EXPECT_FALSE(IsMlDsaPeerIdForm("QmTWkSQAcGsETogTFKrPJ3GzdHG3vP9UwXVyp2HeK9YBvPP")) << "47 characters";
  EXPECT_FALSE(IsMlDsaPeerIdForm("QmTWkSQAcGsETogTFKrPJ3GzdHG3vP9UwXVyp2HeK9YBv0")) << "0 is not base58";
  EXPECT_FALSE(IsMlDsaPeerIdForm("QmTWkSQAcGsETogTFKrPJ3GzdHG3vP9UwXVyp2HeK9YBvl")) << "l is not base58";
  EXPECT_FALSE(IsMlDsaPeerIdForm("12D3KooWD3eckifWpRn9wQpMG9R9hX3sD158z7EqHWmweQAJU5SA")) << "an Ed25519 PeerId";
  EXPECT_FALSE(IsMlDsaPeerIdForm("1mTWkSQAcGsETogTFKrPJ3GzdHG3vP9UwXVyp2HeK9YBvP")) << "a leading zero byte";
  EXPECT_FALSE(IsMlDsaPeerIdForm("zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz")) << "not a SHA-256 multihash";
}

} // namespace
} // namespace pbr
