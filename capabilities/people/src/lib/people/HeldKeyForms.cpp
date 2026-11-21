#include "lib/people/HeldKeyForms.h"

#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/MlDsa.h"
#include "foundation/error/AppError.h"

#include "common/PbrCompat.h"

namespace pbr {

namespace {

/** `held` as a typed blob of `kind`: a bare body of `bare_size` gets this build's id, anything else is read as typed. */
Roe<ByteVector> AsTyped(const pp::BlobKind kind, const size_t bare_size, const std::string& held_b64,
                        const std::string_view what) {
  auto held = Base64Decode(held_b64);
  if (!held) {
    return Error(std::string(what) + ": not base64");
  }
  if (held->size() == bare_size) {
    return WriteTypedBlob(kind, held.value());
  }
  const TypedRead read = ReadTypedBlob(kind, held.value(), what);
  if (!read.ok()) {
    return read.ToError();
  }
  return std::move(held.value());
}

} // namespace

TypedRead ReadHeldSigningKeyB64(const std::string& held_b64, const std::string_view what) {
  auto typed = AsTyped(pp::BlobKind::kSignaturePublicKey, kMlDsa65PublicKeyBytes, held_b64, what);
  if (!typed) {
    TypedRead failed;
    failed.status =
        AppError::IsNewerFormat(typed.error()) ? TypedBlobStatus::UnknownAlgorithm : TypedBlobStatus::Malformed;
    failed.detail = typed.error().message;
    return failed;
  }
  return ReadTypedBlob(pp::BlobKind::kSignaturePublicKey, typed.value(), what);
}

Roe<std::string> TypedSigningKeyB64FromHeld(const std::string& held_b64) {
  auto typed = AsTyped(pp::BlobKind::kSignaturePublicKey, kMlDsa65PublicKeyBytes, held_b64, "signing key");
  if (!typed) {
    return typed.error();
  }
  return Base64Encode(typed.value());
}

Roe<std::string> TypedSignatureB64FromSigner(const std::string& signer_b64) {
  auto typed = AsTyped(pp::BlobKind::kSignature, kMlDsa65SignatureBytes, signer_b64, "signature");
  if (!typed) {
    return typed.error();
  }
  return Base64Encode(typed.value());
}

} // namespace pbr
