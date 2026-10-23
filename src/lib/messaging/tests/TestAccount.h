#pragma once

// A test account for messaging's own tests: the keys a profile's identity holds (people's, which
// messaging does not include), made here from foundation crypto.

#include "lib/messaging/RelayReceivePipeline.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/HybridKem.h"
#include "foundation/crypto/KemKeyBinding.h"
#include "foundation/crypto/MlDsa.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr::test {

struct TestAccount {
  std::string account_id;
  ByteVector signing_public;
  ByteVector signing_secret;
  ByteVector kem_public;
  ByteVector kem_private;
  /** The device's own key (its mesh peer id derives from it), apart from the account's. */
  ByteVector device_public;

  static TestAccount Make() {
    TestAccount a;
    auto signing = MlDsa::GenerateKeyPair();
    EXPECT_TRUE(static_cast<bool>(signing));
    a.signing_public = signing->public_key;
    a.signing_secret = signing->secret_key;
    a.account_id = AccountIdFromMlDsaPublicKey(a.signing_public).value();
    auto kem = HybridKem::GenerateKeyPair();
    EXPECT_TRUE(static_cast<bool>(kem));
    a.kem_public = kem->public_key;
    a.kem_private = kem->private_key;
    auto device = MlDsa::GenerateKeyPair();
    EXPECT_TRUE(static_cast<bool>(device));
    a.device_public = device->public_key;
    return a;
  }

  std::string SigningPublicB64() const { return Base64Encode(signing_public); }
  std::string KemPublicB64() const { return Base64Encode(kem_public); }
  std::string DevicePublicB64() const { return Base64Encode(device_public); }

  /** The account's binding of its KEM key to itself, base64 (as the directory carries it). */
  std::string KemBindingB64() const {
    return Sign(KemBindingSignBytes(SigningPublicB64(), KemPublicB64())).value();
  }

  /** The account key's signature over `bytes`, base64 (as an envelope carries it). */
  Roe<std::string> Sign(const std::vector<uint8_t>& bytes) const {
    auto signature = MlDsa::Sign(signing_secret, bytes);
    if (!signature) {
      return signature.error();
    }
    return Base64Encode(*signature);
  }

  /** The receive pipeline's view of this account as the local one. */
  LocalKeys Keys() const {
    return LocalKeys{[id = account_id]() -> Roe<std::string> { return id; },
                     [kem = kem_private]() -> Roe<ByteVector> { return kem; }};
  }
};

} // namespace pbr::test
