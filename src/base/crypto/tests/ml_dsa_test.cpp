#include "base/crypto/MlDsa.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(MlDsaTest, KeygenSignVerifyAndAccountId) {
  auto keys = MlDsa::GenerateKeyPair();
  ASSERT_TRUE(static_cast<bool>(keys));
  EXPECT_EQ(keys->public_key.size(), kMlDsa65PublicKeyBytes);
  EXPECT_EQ(keys->secret_key.size(), kMlDsa65SecretKeyBytes);

  const ByteVector msg = {'h', 'e', 'l', 'l', 'o'};
  auto sig = MlDsa::Sign(keys->secret_key, msg);
  ASSERT_TRUE(static_cast<bool>(sig));
  EXPECT_EQ(sig->size(), kMlDsa65SignatureBytes);

  auto ok = MlDsa::Verify(keys->public_key, msg, *sig);
  ASSERT_TRUE(static_cast<bool>(ok));
  EXPECT_TRUE(*ok);

  ByteVector bad = *sig;
  bad[0] ^= 0x01;
  auto bad_ok = MlDsa::Verify(keys->public_key, msg, bad);
  ASSERT_TRUE(static_cast<bool>(bad_ok));
  EXPECT_FALSE(*bad_ok);

  auto account_id = AccountIdFromMlDsaPublicKey(keys->public_key);
  ASSERT_TRUE(static_cast<bool>(account_id));
  EXPECT_EQ(account_id->rfind("account:", 0), 0u);
  // base64url-unpadded BLAKE2b-256 → 43 chars after prefix
  EXPECT_EQ(account_id->size(), std::string("account:").size() + 43);
  EXPECT_EQ(account_id->find('+'), std::string::npos);
  EXPECT_EQ(account_id->find('/'), std::string::npos);
  EXPECT_EQ(account_id->find('='), std::string::npos);
}

} // namespace
} // namespace pbr
