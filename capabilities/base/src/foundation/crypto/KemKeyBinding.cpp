#include "foundation/crypto/KemKeyBinding.h"

#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/HybridKem.h"
#include "foundation/crypto/MlDsa.h"


namespace pbr {

namespace {

constexpr char kKemBindingDomain[] = "pp-browser:kem-binding-v1";
constexpr uint8_t kKemBindingVersion = 1;

} // namespace

ByteVector KemBindingSignBytes(const std::string& signing_public_key_b64, const std::string& kem_public_key_b64) {
  auto signing_public_key = Base64Decode(signing_public_key_b64);
  auto kem_public_key = Base64Decode(kem_public_key_b64);
  if (!signing_public_key || signing_public_key.value().size() != kMlDsa65PublicKeyBytes || !kem_public_key ||
      kem_public_key.value().size() != kHybridKemPublicKeyBytes) {
    return {};
  }
  const ByteVector& signing = signing_public_key.value();
  const ByteVector& kem = kem_public_key.value();
  ByteVector bytes;
  bytes.reserve(sizeof(kKemBindingDomain) + 1 + signing.size() + kem.size());
  bytes.insert(bytes.end(), kKemBindingDomain, kKemBindingDomain + sizeof(kKemBindingDomain)); // with its NUL
  bytes.push_back(kKemBindingVersion);
  bytes.insert(bytes.end(), signing.begin(), signing.end());
  bytes.insert(bytes.end(), kem.begin(), kem.end());
  return bytes;
}

Roe<void> VerifyKemBinding(const std::string& account_id, const std::string& signing_public_key_b64,
                           const std::string& kem_public_key_b64, const std::string& binding_signature_b64) {
  auto signing_public_key = Base64Decode(signing_public_key_b64);
  if (!signing_public_key) {
    return Error("KEM key binding: bad signing key");
  }
  auto derived = AccountIdFromMlDsaPublicKey(signing_public_key.value());
  if (!derived || derived.value() != account_id) {
    return Error("KEM key binding: the signing key is not the account's");
  }
  const ByteVector message = KemBindingSignBytes(signing_public_key_b64, kem_public_key_b64);
  auto signature = Base64Decode(binding_signature_b64);
  if (message.empty() || !signature || signature.value().size() != kMlDsa65SignatureBytes) {
    return Error("KEM key binding: malformed");
  }
  auto verified = MlDsa::Verify(signing_public_key.value(), message, signature.value());
  if (!verified || !verified.value()) {
    return Error("KEM key binding: the account did not sign this KEM key");
  }
  return {};
}

} // namespace pbr
