#include "lib/people/CardKeys.h"

#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/KemKeyBinding.h"
#include "foundation/crypto/TypedBlob.h"

#include <utility>

namespace pbr {

namespace {

Roe<ByteVector> RawKey(const pp::BlobKind kind, const std::string& typed_b64, const std::string_view what) {
  TypedRead read = ReadTypedBlobB64(kind, typed_b64, what);
  if (!read.ok()) {
    return read.ToError();
  }
  return std::move(read.raw);
}

} // namespace

Roe<std::string> TypedKeyB64(const pp::BlobKind kind, const std::string& raw_b64) {
  if (raw_b64.empty()) {
    return Error("key not set");
  }
  auto raw = Base64Decode(raw_b64);
  if (!raw) {
    return Error("key is not base64");
  }
  return WriteTypedBlobB64(kind, *raw);
}

Roe<std::string> TypedSignatureB64(const std::string& raw_signature_b64) {
  return TypedKeyB64(pp::BlobKind::kSignature, raw_signature_b64);
}

Roe<ByteVector> RawSigningPublicKey(const std::string& typed_b64, const std::string_view what) {
  return RawKey(pp::BlobKind::kSignaturePublicKey, typed_b64, what);
}

Roe<ByteVector> RawKemPublicKey(const std::string& typed_b64, const std::string_view what) {
  return RawKey(pp::BlobKind::kKemPublicKey, typed_b64, what);
}

Roe<std::string> RawSigningPublicKeyB64(const std::string& typed_b64, const std::string_view what) {
  auto raw = RawSigningPublicKey(typed_b64, what);
  if (!raw) {
    return raw.error();
  }
  return Base64Encode(*raw);
}

Roe<std::string> RawKemPublicKeyB64(const std::string& typed_b64, const std::string_view what) {
  auto raw = RawKemPublicKey(typed_b64, what);
  if (!raw) {
    return raw.error();
  }
  return Base64Encode(*raw);
}

Roe<KeyCard> MakeKeyCard(const std::string& account_id, const std::string& raw_signing_public_key_b64,
                         const std::string& raw_kem_public_key_b64, const AccountRawSignFn& sign) {
  auto signing = TypedKeyB64(pp::BlobKind::kSignaturePublicKey, raw_signing_public_key_b64);
  if (!signing) {
    return Error("key card: the account signing key: " + signing.error().message);
  }
  auto kem = TypedKeyB64(pp::BlobKind::kKemPublicKey, raw_kem_public_key_b64);
  if (!kem) {
    return Error("key card: the account KEM key: " + kem.error().message);
  }
  auto binding = SignKemBindingB64(*signing, *kem, sign);
  if (!binding) {
    return binding.error();
  }
  return KeyCard{.account_id = account_id,
                 .signing_public_key_b64 = std::move(*signing),
                 .kem_public_key_b64 = std::move(*kem),
                 .kem_binding_b64 = std::move(*binding)};
}

} // namespace pbr
