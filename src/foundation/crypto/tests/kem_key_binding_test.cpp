#include "foundation/crypto/KemKeyBinding.h"

#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/HybridKem.h"
#include "foundation/crypto/MlDsa.h"

#include <gtest/gtest.h>

#include <string>

namespace pbr {
namespace {

struct Account {
  MlDsaKeyPair signing;
  std::string id;
  std::string signing_b64;
  std::string kem_b64;

  static Account Make() {
    Account a;
    a.signing = MlDsa::GenerateKeyPair().value();
    a.id = AccountIdFromMlDsaPublicKey(a.signing.public_key).value();
    a.signing_b64 = Base64Encode(a.signing.public_key);
    a.kem_b64 = Base64Encode(HybridKem::GenerateKeyPair().value().public_key);
    return a;
  }

  std::string Bind(const std::string& kem_b64) const {
    return Base64Encode(MlDsa::Sign(signing.secret_key, KemBindingSignBytes(signing_b64, kem_b64)).value());
  }
};

TEST(KemKeyBindingTest, SignBytesAreTheDomainVersionAndBothKeys) {
  const Account a = Account::Make();
  const ByteVector bytes = KemBindingSignBytes(a.signing_b64, a.kem_b64);
  const std::string domain = "pp-browser:kem-binding-v1";
  ASSERT_EQ(bytes.size(), domain.size() + 1 + 1 + kMlDsa65PublicKeyBytes + kHybridKemPublicKeyBytes);
  EXPECT_EQ(std::string(bytes.begin(), bytes.begin() + static_cast<long>(domain.size())), domain);
  EXPECT_EQ(bytes[domain.size()], 0);
  EXPECT_EQ(bytes[domain.size() + 1], 1);
  EXPECT_TRUE(std::equal(a.signing.public_key.begin(), a.signing.public_key.end(),
                         bytes.begin() + static_cast<long>(domain.size() + 2)));
  EXPECT_TRUE(KemBindingSignBytes(a.signing_b64, "c2hvcnQ=").empty()) << "a KEM key of the wrong size";
}

TEST(KemKeyBindingTest, TheAccountsOwnBindingVerifies) {
  const Account a = Account::Make();
  EXPECT_TRUE(VerifyKemBinding(a.id, a.signing_b64, a.kem_b64, a.Bind(a.kem_b64)));
}

TEST(KemKeyBindingTest, AKeyTheAccountDidNotBindIsRefused) {
  const Account a = Account::Make();
  const Account other = Account::Make();
  // Another KEM key under the account's binding.
  EXPECT_FALSE(VerifyKemBinding(a.id, a.signing_b64, other.kem_b64, a.Bind(a.kem_b64)));
  // Someone else's key and binding, claimed for this account.
  EXPECT_FALSE(VerifyKemBinding(a.id, other.signing_b64, other.kem_b64, other.Bind(other.kem_b64)));
  // The account's signing key with a binding someone else made.
  EXPECT_FALSE(VerifyKemBinding(a.id, a.signing_b64, other.kem_b64, other.Bind(other.kem_b64)));
  EXPECT_FALSE(VerifyKemBinding(a.id, a.signing_b64, a.kem_b64, ""));
  EXPECT_FALSE(VerifyKemBinding(a.id, a.signing_b64, a.kem_b64, "bm90IGEgc2lnbmF0dXJl"));
}

} // namespace
} // namespace pbr
