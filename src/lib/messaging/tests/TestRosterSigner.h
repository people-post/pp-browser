#pragma once

// An account that signs roster statements (G012) with its own ML-DSA key, as IdentityStore does.

#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/MlDsa.h"
#include "common/directory/IAccountSigningAccess.h"

#include <gtest/gtest.h>

#include <string>
#include "common/PbrCompat.h"

namespace pbr::test {

/** An account that signs with its own ML-DSA key, as IdentityStore does. */
class TestSigner final : public IAccountSigningAccess {
public:
  TestSigner() {
    auto keys = MlDsa::GenerateKeyPair();
    EXPECT_TRUE(static_cast<bool>(keys));
    public_key_ = keys->public_key;
    secret_key_ = keys->secret_key;
    account_id_ = AccountIdFromMlDsaPublicKey(public_key_).value();
  }
  Roe<std::string> GetAccountId() const override { return account_id_; }
  Roe<std::string> SignAccountBytes(const AccountSignBytes& sign_bytes) const override {
    auto signature = MlDsa::Sign(secret_key_, sign_bytes);
    if (!signature) {
      return signature.error();
    }
    return Base64Encode(*signature);
  }
  std::string Key() const { return Base64Encode(public_key_); }
  const std::string& Id() const { return account_id_; }

private:
  ByteVector public_key_;
  ByteVector secret_key_;
  std::string account_id_;
};

} // namespace pbr::test
