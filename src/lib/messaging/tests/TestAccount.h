#pragma once

// A test account for messaging's own tests: the keys a profile's identity holds (people's, which
// messaging does not include), made here from foundation crypto.

#include "lib/messaging/AccountCard.h"
#include "lib/messaging/RelayReceivePipeline.h"
#include "crypto/PeerId.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/HybridKem.h"
#include "common/directory/KeyCard.h"
#include "foundation/crypto/KemKeyBinding.h"
#include "foundation/crypto/MlDsa.h"

#include <gtest/gtest.h>

#include <initializer_list>
#include <map>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr::test {

/** One device of an account (M021): its own signing key (its PeerId comes from it) and its own KEM key. */
struct TestDevice {
  ByteVector signing_public;
  ByteVector signing_secret;
  ByteVector kem_public;
  ByteVector kem_private;
  std::string peer_id;

  static TestDevice Make() {
    TestDevice d;
    auto signing = MlDsa::GenerateKeyPair();
    EXPECT_TRUE(static_cast<bool>(signing));
    d.signing_public = signing->public_key;
    d.signing_secret = signing->secret_key;
    auto kem = HybridKem::GenerateKeyPair();
    EXPECT_TRUE(static_cast<bool>(kem));
    d.kem_public = kem->public_key;
    d.kem_private = kem->private_key;
    d.peer_id = pp::PeerIdFromMlDsaPublicKey(d.signing_public).value();
    return d;
  }

  /** The device as an account card lists it. */
  AccountCardDevice Entry() const {
    return AccountCardDevice{.peer_id = peer_id,
                             .signing_public_key_b64 = Base64Encode(signing_public),
                             .kem_public_key_b64 = Base64Encode(kem_public)};
  }

  /** The device key's signature over `bytes`, base64 (as a device signs its envelopes). */
  Roe<std::string> Sign(const std::vector<uint8_t>& bytes) const {
    auto signature = MlDsa::Sign(signing_secret, bytes);
    if (!signature) {
      return signature.error();
    }
    return Base64Encode(*signature);
  }
};

/** The same test device for the same name, within one test binary (keys are slow to make). */
inline const TestDevice& NamedDevice(const std::string& name) {
  static std::map<std::string, TestDevice> devices;
  auto found = devices.find(name);
  if (found == devices.end()) {
    found = devices.emplace(name, TestDevice::Make()).first;
  }
  return found->second;
}

/** Card entries for named test devices. */
inline std::vector<AccountCardDevice> Devices(std::initializer_list<std::string> names) {
  std::vector<AccountCardDevice> out;
  for (const std::string& name : names) {
    out.push_back(NamedDevice(name).Entry());
  }
  return out;
}

/** A named test device's PeerId. */
inline std::string PeerIdOf(const std::string& name) {
  return NamedDevice(name).peer_id;
}

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

  /** The account's key card (as the directory or a first message carries it). */
  KeyCard Card() const {
    return KeyCard{.account_id = account_id,
                   .signing_public_key_b64 = SigningPublicB64(),
                   .kem_public_key_b64 = KemPublicB64(),
                   .kem_binding_signature_b64 = KemBindingB64()};
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
