#include "foundation/crypto/TypedBlob.h"

#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/MlDsa.h"
#include "foundation/error/AppError.h"

namespace pbr {

namespace {

std::string Named(std::string_view what, std::string_view why) {
  std::string out(what);
  out += ": ";
  out += why;
  return out;
}

TypedRead Failed(TypedBlobStatus status, std::string detail) {
  TypedRead read;
  read.status = status;
  read.detail = std::move(detail);
  return read;
}

} // namespace

Error TypedRead::ToError() const {
  if (status == TypedBlobStatus::UnknownAlgorithm) {
    return AppError::NewerFormat(detail);
  }
  return Error(detail.empty() ? std::string("typed blob: malformed") : detail);
}

TypedRead ReadTypedBlob(const pp::BlobKind kind, const std::span<const uint8_t> blob, const std::string_view what) {
  auto decoded = pp::DecodeTypedBlob(kind, blob);
  if (!decoded) {
    const bool unknown = pp::IsUnknownAlgorithm(decoded.error());
    const std::string why = unknown ? "unknown " + std::string(pp::AlgKindName(pp::BlobAlgKind(kind))) +
                                          " algorithm id " + std::to_string(blob.empty() ? 0 : blob[0])
                                    : "malformed " + std::string(pp::BlobKindName(kind));
    return Failed(unknown ? TypedBlobStatus::UnknownAlgorithm : TypedBlobStatus::Malformed, Named(what, why));
  }
  TypedRead read;
  read.status = TypedBlobStatus::Ok;
  read.alg_id = decoded.value().alg->id;
  read.raw = std::move(decoded.value().body);
  return read;
}

TypedRead ReadTypedBlobB64(const pp::BlobKind kind, const std::string& blob_b64, const std::string_view what) {
  auto blob = Base64Decode(blob_b64);
  if (!blob) {
    return Failed(TypedBlobStatus::Malformed, Named(what, "not base64"));
  }
  return ReadTypedBlob(kind, blob.value(), what);
}

uint8_t WrittenAlgId(const pp::BlobKind kind) {
  switch (pp::BlobAlgKind(kind)) {
    case pp::AlgKind::kSignature:
      return pp::kSignatureAlgMlDsa65;
    case pp::AlgKind::kKem:
      return pp::kKemAlgMlKem768;
    case pp::AlgKind::kAead:
      return pp::kAeadAlgXChaCha20Poly1305;
    case pp::AlgKind::kHash:
    case pp::AlgKind::kKdf:
      break;
  }
  return pp::kAlgIdInvalid;
}

Roe<ByteVector> WriteTypedBlob(const pp::BlobKind kind, const std::span<const uint8_t> raw) {
  return pp::EncodeTypedBlob(kind, WrittenAlgId(kind), raw);
}

Roe<std::string> WriteTypedBlobB64(const pp::BlobKind kind, const std::span<const uint8_t> raw) {
  auto blob = WriteTypedBlob(kind, raw);
  if (!blob) {
    return blob.error();
  }
  return Base64Encode(blob.value());
}

Roe<ByteVector> SignTyped(const ByteVector& secret_key, const ByteVector& message) {
  auto signature = MlDsa::Sign(secret_key, message);
  if (!signature) {
    return signature.error();
  }
  return TypedSignature(signature.value());
}

Roe<void> VerifyTyped(const TypedRead& public_key, const ByteVector& message, const TypedRead& signature) {
  if (!public_key.ok() || !signature.ok()) {
    return Error("signature: the key or the signature was not read");
  }
  if (public_key.alg_id != signature.alg_id) {
    return Error("signature: its algorithm is not the key's");
  }
  if (public_key.alg_id != pp::kSignatureAlgMlDsa65) {
    return Error("signature: no verifier for algorithm id " + std::to_string(public_key.alg_id));
  }
  auto verified = MlDsa::Verify(public_key.raw, message, signature.raw);
  if (!verified || !verified.value()) {
    return Error("signature: does not verify");
  }
  return {};
}

} // namespace pbr
