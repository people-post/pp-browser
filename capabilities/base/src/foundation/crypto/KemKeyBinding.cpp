#include "foundation/crypto/KemKeyBinding.h"

#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/MlDsa.h"
#include "foundation/crypto/SignedBytes.h"
#include "foundation/crypto/TypedBlob.h"

#include "common/Serialize.hpp"

#include <sstream>

namespace pbr {

ByteVector KemBindingSignBytes(const std::span<const uint8_t> typed_signing_public_key,
                               const std::span<const uint8_t> typed_kem_public_key, const pp::ExtArea& ext) {
  return SignedBytesWriter(kKemBindingDomain, kKemBindingVersion)
      .Typed(typed_signing_public_key)
      .Typed(typed_kem_public_key)
      .Finish(ext);
}

ByteVector KemBindingBlob(const std::span<const uint8_t> typed_signature, const pp::ExtArea& ext) {
  ByteVector blob{kKemBindingVersion};
  ext.AppendTo(blob);
  std::ostringstream out;
  pp::OutputArchive ar(out);
  pp::WireLenBytes signature{ByteVector(typed_signature.begin(), typed_signature.end())};
  ar & signature;
  const std::string packed = out.str();
  blob.insert(blob.end(), packed.begin(), packed.end());
  return blob;
}

Roe<std::string> SignKemBindingB64(const std::string& typed_signing_public_key_b64,
                                   const std::string& typed_kem_public_key_b64, const KemBindingSignFn& sign,
                                   const pp::ExtArea& ext) {
  const TypedRead signing =
      ReadTypedBlobB64(pp::BlobKind::kSignaturePublicKey, typed_signing_public_key_b64, "KEM binding signing key");
  const TypedRead kem = ReadTypedBlobB64(pp::BlobKind::kKemPublicKey, typed_kem_public_key_b64, "KEM binding KEM key");
  if (!signing.ok()) {
    return signing.ToError();
  }
  if (!kem.ok()) {
    return kem.ToError();
  }
  auto typed_signing = Base64Decode(typed_signing_public_key_b64);
  auto typed_kem = Base64Decode(typed_kem_public_key_b64);
  if (!typed_signing || !typed_kem) {
    return Error("KEM binding: a key is not base64");
  }
  auto signature_b64 = sign(KemBindingSignBytes(*typed_signing, *typed_kem, ext));
  if (!signature_b64) {
    return signature_b64.error();
  }
  auto signature_raw = Base64Decode(*signature_b64);
  if (!signature_raw) {
    return Error("KEM binding: the signature is not base64");
  }
  auto signature = TypedSignature(*signature_raw);
  if (!signature) {
    return signature.error();
  }
  return Base64Encode(KemBindingBlob(*signature, ext));
}

SignedReceipt ReceiveKemBinding(const std::string& typed_signing_public_key_b64,
                                const std::string& typed_kem_public_key_b64, const std::string& binding_b64) {
  ByteVector blob;
  if (!binding_b64.empty()) {
    auto decoded = Base64Decode(binding_b64);
    if (!decoded) {
      return SignedReceipt{.verdict = SignedVerdict::Malformed, .detail = "KEM binding: not base64"};
    }
    blob = std::move(decoded.value());
  }
  pp::ExtArea ext;
  ByteVector typed_signature;
  TypedRead signing;
  TypedRead kem;
  TypedRead signature;
  return ReceiveSigned(SignedReceiveSteps{
      .what = "KEM binding",
      .outside_version = OutsideVersionOfBlob(blob),
      .versions = {.floor = kKemBindingVersion, .current = kKemBindingVersion},
      .parse = [&]() -> Roe<void> {
        SignedBlobReader in(blob);
        (void)in.U8(); // the version, read outside
        ext = in.Ext();
        typed_signature = in.Bytes();
        if (!in.Done()) {
          return Error("its area or signature does not parse, or bytes follow them");
        }
        return {};
      },
      .algorithms = [&]() -> Roe<void> {
        signing = ReadTypedBlobB64(pp::BlobKind::kSignaturePublicKey, typed_signing_public_key_b64, "signing key");
        if (!signing.ok()) {
          return signing.ToError();
        }
        kem = ReadTypedBlobB64(pp::BlobKind::kKemPublicKey, typed_kem_public_key_b64, "KEM key");
        if (!kem.ok()) {
          return kem.ToError();
        }
        signature = ReadTypedBlob(pp::BlobKind::kSignature, typed_signature, "signature");
        if (!signature.ok()) {
          return signature.ToError();
        }
        return {};
      },
      .verify = [&]() -> Roe<void> {
        auto typed_signing = Base64Decode(typed_signing_public_key_b64);
        auto typed_kem = Base64Decode(typed_kem_public_key_b64);
        if (!typed_signing || !typed_kem) {
          return Error("a key is not base64");
        }
        if (auto verified = VerifyTyped(signing, KemBindingSignBytes(*typed_signing, *typed_kem, ext), signature);
            !verified) {
          return Error("the account did not sign this KEM key");
        }
        return {};
      },
      .ext = &ext,
  });
}

Roe<void> VerifyKemBinding(const std::string& account_id, const std::string& typed_signing_public_key_b64,
                           const std::string& typed_kem_public_key_b64, const std::string& binding_b64) {
  const SignedReceipt receipt = ReceiveKemBinding(typed_signing_public_key_b64, typed_kem_public_key_b64, binding_b64);
  if (!receipt.accepted()) {
    return receipt.ToError();
  }
  // Step 6: the key that signed is the account's (an Account ID hashes the raw key).
  const TypedRead signing =
      ReadTypedBlobB64(pp::BlobKind::kSignaturePublicKey, typed_signing_public_key_b64, "signing key");
  auto derived = AccountIdFromMlDsaPublicKey(signing.raw);
  if (!derived || derived.value() != account_id) {
    return Error("KEM binding: the signing key is not the account's");
  }
  return {};
}

} // namespace pbr
