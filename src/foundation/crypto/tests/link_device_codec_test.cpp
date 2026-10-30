#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/HybridKem.h"
#include "foundation/crypto/LinkDeviceCodec.h"
#include "foundation/crypto/MlDsa.h"
#include "foundation/crypto/RecoverySeed.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

ByteVector TestDek() {
  ByteVector dek(kDataEncryptionKeySize);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0xa0 + i);
  }
  return dek;
}

LinkDeviceBundleV1 MakeValidBundle() {
  auto keys = MlDsa::GenerateKeyPair();
  EXPECT_TRUE(keys);
  auto account_id = AccountIdFromMlDsaPublicKey(keys->public_key);
  EXPECT_TRUE(account_id);
  LinkDeviceBundleV1 bundle;
  bundle.account_id = *account_id;
  bundle.account_ml_dsa_pk_b64 = Base64Encode(keys->public_key);
  bundle.account_ml_dsa_sk_b64 = Base64Encode(keys->secret_key);
  auto kem = HybridKem::GenerateKeyPair();
  EXPECT_TRUE(kem);
  bundle.account_kem_pk_b64 = Base64Encode(kem->public_key);
  bundle.account_kem_sk_b64 = Base64Encode(kem->private_key);
  bundle.dek_b64 = Base64Encode(TestDek());
  bundle.relay_user_id = "relay:alice";
  bundle.nickname = "alice";
  bundle.created_at_ms = 1'700'000'000'000;
  bundle.expires_at_ms = bundle.created_at_ms + kLinkDeviceDefaultTtlMs;
  return bundle;
}

} // namespace

TEST(LinkDeviceCodecTest, SerializeParseRoundTrip) {
  LinkDeviceBundleV1 bundle = MakeValidBundle();
  LinkDevicePublicPsk row;
  row.key.peer_identity_kind = "account";
  row.key.peer_identity_value = "account:bob";
  row.key.channel = CryptoChannel::E2ePublic;
  row.session_epoch = 2;
  row.master_psk_b64 = Base64Encode(TestDek());
  bundle.public_psks.push_back(row);

  auto json = LinkDeviceCodec::Serialize(bundle);
  ASSERT_TRUE(json) << json.error().message;
  auto parsed = LinkDeviceCodec::Parse(*json);
  ASSERT_TRUE(parsed) << parsed.error().message;
  EXPECT_EQ(parsed->account_id, bundle.account_id);
  EXPECT_EQ(parsed->account_kem_pk_b64, bundle.account_kem_pk_b64);
  EXPECT_EQ(parsed->relay_user_id, "relay:alice");
  ASSERT_EQ(parsed->public_psks.size(), 1u);
  EXPECT_EQ(parsed->public_psks.front().key.peer_identity_value, "account:bob");
  EXPECT_EQ(parsed->public_psks.front().key.channel, CryptoChannel::E2ePublic);
}

TEST(LinkDeviceCodecTest, TheIssuersAccountCardRidesAlongWhenGiven) {
  LinkDeviceBundleV1 bundle = MakeValidBundle();
  auto without = LinkDeviceCodec::Parse(LinkDeviceCodec::Serialize(bundle).value());
  ASSERT_TRUE(without);
  EXPECT_FALSE(without->account_card_json.has_value()) << "a backup carries none";
  bundle.account_card_json = R"({"card_version":2})";
  auto with = LinkDeviceCodec::Parse(LinkDeviceCodec::Serialize(bundle).value());
  ASSERT_TRUE(with);
  EXPECT_EQ(with->account_card_json, bundle.account_card_json) << "as given: the card checks itself on import";
}

/** A bundle for an account whose keys derive from `seed` (M023), carrying the seed. */
LinkDeviceBundleV1 BundleFromSeed(const ByteVector& seed) {
  LinkDeviceBundleV1 bundle = MakeValidBundle();
  const RecoveryKeys keys = DeriveRecoveryKeys(seed).value();
  auto signing = MlDsa::GenerateKeyPairFromSeed(keys.account_ml_dsa_seed).value();
  auto kem = HybridKem::GenerateKeyPairFromSeed(keys.account_ml_kem_coins).value();
  bundle.account_id = AccountIdFromMlDsaPublicKey(signing.public_key).value();
  bundle.account_ml_dsa_pk_b64 = Base64Encode(signing.public_key);
  bundle.account_ml_dsa_sk_b64 = Base64Encode(signing.secret_key);
  bundle.account_kem_pk_b64 = Base64Encode(kem.public_key);
  bundle.account_kem_sk_b64 = Base64Encode(kem.private_key);
  bundle.recovery_seed_b64 = Base64Encode(seed);
  return bundle;
}

TEST(LinkDeviceCodecTest, TheRecoverySeedTravelsOnlyWithTheKeysItDerives) {
  const ByteVector seed = GenerateRecoverySeed();
  auto json = LinkDeviceCodec::Serialize(BundleFromSeed(seed));
  ASSERT_TRUE(json) << json.error().message;
  auto parsed = LinkDeviceCodec::Parse(*json);
  ASSERT_TRUE(parsed) << parsed.error().message;
  ASSERT_TRUE(parsed->recovery_seed_b64);
  EXPECT_EQ(*parsed->recovery_seed_b64, Base64Encode(seed)) << "the new device shows the same phrase";

  // Another account's seed with these keys: its phrase would restore someone else.
  LinkDeviceBundleV1 mixed = BundleFromSeed(seed);
  mixed.recovery_seed_b64 = Base64Encode(GenerateRecoverySeed());
  EXPECT_FALSE(LinkDeviceCodec::Validate(mixed));
  // An account from before M023: random keys, no seed — still links.
  LinkDeviceBundleV1 older = MakeValidBundle();
  EXPECT_FALSE(older.recovery_seed_b64);
  EXPECT_TRUE(LinkDeviceCodec::Validate(older));
}

TEST(LinkDeviceCodecTest, RejectsPrivateE2ePsks) {
  LinkDeviceBundleV1 bundle = MakeValidBundle();
  LinkDevicePublicPsk row;
  row.key.peer_identity_kind = "account";
  row.key.peer_identity_value = "account:bob";
  row.key.channel = CryptoChannel::E2e;
  row.session_epoch = 1;
  row.master_psk_b64 = Base64Encode(TestDek());
  bundle.public_psks.push_back(row);
  auto json = LinkDeviceCodec::Serialize(bundle);
  EXPECT_FALSE(json);
}

TEST(LinkDeviceCodecTest, RejectsExpiredBundle) {
  LinkDeviceBundleV1 bundle = MakeValidBundle();
  auto json = LinkDeviceCodec::Serialize(bundle);
  ASSERT_TRUE(json);
  auto parsed = LinkDeviceCodec::Parse(*json);
  ASSERT_TRUE(parsed);
  auto expired = LinkDeviceCodec::Validate(*parsed, bundle.expires_at_ms + 1);
  EXPECT_FALSE(expired);
}

TEST(LinkDeviceCodecTest, RejectsAccountIdMismatch) {
  LinkDeviceBundleV1 bundle = MakeValidBundle();
  bundle.account_id = "account:not-the-key";
  auto json = LinkDeviceCodec::Serialize(bundle);
  EXPECT_FALSE(json);
}

TEST(LinkDeviceCodecTest, RejectsMissingAccountKem) {
  LinkDeviceBundleV1 bundle = MakeValidBundle();
  bundle.account_kem_pk_b64.clear();
  auto json = LinkDeviceCodec::Serialize(bundle);
  EXPECT_FALSE(json);
}

} // namespace pbr
