#pragma once

#include "lib/posts/PostEntry.h"

#include "foundation/crypto/MlDsa.h"

#include <gtest/gtest.h>

#include <string>
#include "common/PbrCompat.h"

namespace pbr::test {

/** An account for post tests: its id, its signing key, and a signer. */
struct PostTestAccount {
  std::string account_id;
  ByteVector public_key;
  ByteVector secret_key;

  static PostTestAccount Make() {
    PostTestAccount a;
    auto keys = MlDsa::GenerateKeyPair();
    EXPECT_TRUE(static_cast<bool>(keys));
    a.public_key = keys.value().public_key;
    a.secret_key = keys.value().secret_key;
    a.account_id = AccountIdFromMlDsaPublicKey(a.public_key).value();
    return a;
  }

  PostSigner Signer() const {
    return [secret = secret_key](const ByteVector& message) { return MlDsa::Sign(secret, message); };
  }
};

} // namespace pbr::test
