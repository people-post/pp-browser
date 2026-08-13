#include "base/crypto/CryptoConstants.h"
#include "base/crypto/CryptoUtil.h"
#include "base/crypto/LinkDeviceCodec.h"
#include "base/crypto/MlDsa.h"

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
  EXPECT_EQ(parsed->relay_user_id, "relay:alice");
  ASSERT_EQ(parsed->public_psks.size(), 1u);
  EXPECT_EQ(parsed->public_psks.front().key.peer_identity_value, "account:bob");
  EXPECT_EQ(parsed->public_psks.front().key.channel, CryptoChannel::E2ePublic);
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

} // namespace pbr
