// The recovery seed and its phrase (multi-device M023 §2): BIP-0039 words for the seed itself, and what it derives.
#include "foundation/crypto/HybridKem.h"
#include "foundation/crypto/MlDsa.h"
#include "foundation/crypto/RecoveryPhrase.h"
#include "foundation/crypto/RecoverySeed.h"

#include "crypto/PeerId.h"

#include <gtest/gtest.h>

#include <set>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

ByteVector Filled(uint8_t byte) { return ByteVector(kRecoverySeedBytes, byte); }

// BIP-0039 reference vectors for 256-bit entropy (trezor/python-mnemonic vectors.json).
TEST(RecoveryPhraseTest, MatchesTheBip39Vectors) {
  EXPECT_EQ(*EncodeRecoveryPhrase(Filled(0x00)),
            "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon "
            "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon art");
  EXPECT_EQ(*EncodeRecoveryPhrase(Filled(0x7f)),
            "legal winner thank year wave sausage worth useful legal winner thank year wave sausage worth useful "
            "legal winner thank year wave sausage worth title");
  EXPECT_EQ(*EncodeRecoveryPhrase(Filled(0x80)),
            "letter advice cage absurd amount doctor acoustic avoid letter advice cage absurd amount doctor acoustic "
            "avoid letter advice cage absurd amount doctor acoustic bless");
  EXPECT_EQ(*EncodeRecoveryPhrase(Filled(0xff)),
            "zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo vote");
}

TEST(RecoveryPhraseTest, ATypedPhraseComesBackAsTheSeed) {
  for (int i = 0; i < 20; ++i) {
    const ByteVector seed = GenerateRecoverySeed();
    const std::string phrase = EncodeRecoveryPhrase(seed).value();
    EXPECT_EQ(DecodeRecoveryPhrase(phrase).value(), seed);
  }
  // As people type it: other spacing, capitals, a trailing newline.
  auto typed = DecodeRecoveryPhrase("  Legal winner THANK year wave sausage worth useful\nlegal winner thank year wave "
                                    "sausage worth useful legal winner thank year wave sausage worth title\n");
  ASSERT_TRUE(typed) << typed.error().message;
  EXPECT_EQ(*typed, Filled(0x7f));
}

TEST(RecoveryPhraseTest, AMistypedPhraseIsRefusedAndSaysWhy) {
  const std::string good = EncodeRecoveryPhrase(Filled(0x7f)).value();
  EXPECT_FALSE(DecodeRecoveryPhrase("legal winner thank")) << "too few words";
  auto unknown = DecodeRecoveryPhrase("legal winnar thank year wave sausage worth useful legal winner thank year wave "
                                      "sausage worth useful legal winner thank year wave sausage worth title");
  ASSERT_FALSE(unknown);
  EXPECT_NE(unknown.error().message.find("word 2"), std::string::npos) << unknown.error().message;
  // Two words swapped: every word is real, the checksum catches it.
  auto swapped = DecodeRecoveryPhrase("winner legal thank year wave sausage worth useful legal winner thank year wave "
                                      "sausage worth useful legal winner thank year wave sausage worth title");
  ASSERT_FALSE(swapped);
  EXPECT_NE(swapped.error().message.find("does not check out"), std::string::npos);
  EXPECT_FALSE(EncodeRecoveryPhrase(ByteVector(16, 0))) << "only a 256-bit seed";
}

TEST(RecoverySeedTest, TheSameSeedDerivesTheSameAccountEveryTime) {
  const ByteVector seed = Filled(0x42);
  const RecoveryKeys a = DeriveRecoveryKeys(seed).value();
  const RecoveryKeys b = DeriveRecoveryKeys(seed).value();
  EXPECT_EQ(a.account_ml_dsa_seed, b.account_ml_dsa_seed);
  EXPECT_EQ(a.locker_key, b.locker_key);
  const auto first = MlDsa::GenerateKeyPairFromSeed(a.account_ml_dsa_seed).value();
  const auto again = MlDsa::GenerateKeyPairFromSeed(b.account_ml_dsa_seed).value();
  EXPECT_EQ(AccountIdFromMlDsaPublicKey(first.public_key).value(), AccountIdFromMlDsaPublicKey(again.public_key).value());
  EXPECT_EQ(HybridKem::GenerateKeyPairFromSeed(a.account_ml_kem_coins)->public_key,
            HybridKem::GenerateKeyPairFromSeed(b.account_ml_kem_coins)->public_key);

  // Every label derives something else, and another seed derives another account.
  const std::set<ByteVector> parts{a.account_ml_dsa_seed, ByteVector(a.account_ml_kem_coins.begin(),
                                                                     a.account_ml_kem_coins.begin() + 32),
                                   a.locker_key, a.locker_signing_seed};
  EXPECT_EQ(parts.size(), 4u);
  EXPECT_NE(DeriveRecoveryKeys(Filled(0x43))->account_ml_dsa_seed, a.account_ml_dsa_seed);
  EXPECT_FALSE(DeriveRecoveryKeys(ByteVector(31, 0)));
}

} // namespace
} // namespace pbr
