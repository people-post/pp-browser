// A recovery phrase restore as a link import (multi-device M023 §8): the bundle the phrase reconstructs.
#include "lib/people/RecoveryBundle.h"

#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/LinkDeviceCodec.h"
#include "foundation/crypto/MlDsa.h"
#include "foundation/crypto/RecoveryPhrase.h"
#include "foundation/crypto/RecoverySeed.h"

#include <gtest/gtest.h>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

TEST(RecoveryBundleTest, ThePhraseRebuildsTheSameAccountWithAFreshDataKey) {
  const ByteVector seed = GenerateRecoverySeed();
  auto first = BuildRecoveryBundleJson(seed, "relay:alice", "alice", 1'700'000'000'000);
  auto second = BuildRecoveryBundleJson(seed, "relay:alice", "alice", 1'700'000'000'000);
  ASSERT_TRUE(first) << first.error().message;
  ASSERT_TRUE(second);
  auto a = LinkDeviceCodec::Parse(*first).value();
  auto b = LinkDeviceCodec::Parse(*second).value();
  EXPECT_TRUE(LinkDeviceCodec::Validate(a, 1'700'000'000'001)) << "an ordinary bundle the import takes";
  EXPECT_EQ(a.account_id, b.account_id) << "the phrase alone names the account";
  const auto signing = MlDsa::GenerateKeyPairFromSeed(DeriveRecoveryKeys(seed)->account_ml_dsa_seed).value();
  EXPECT_EQ(a.account_id, AccountIdFromMlDsaPublicKey(signing.public_key).value());
  EXPECT_EQ(a.recovery_seed_b64, std::optional<std::string>(Base64Encode(seed))) << "the restored install keeps it";
  EXPECT_NE(a.dek_b64, b.dek_b64) << "nothing here was sealed under the old data key: a new one";
  EXPECT_FALSE(a.account_card_json) << "the restored install signs a fresh device list";
  EXPECT_FALSE(BuildRecoveryBundleJson(seed, "", "alice", 1)) << "no relay binding: no account to come back to";
}

} // namespace
} // namespace pbr
