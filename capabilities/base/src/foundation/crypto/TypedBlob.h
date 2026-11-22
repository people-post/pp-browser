#pragma once

#include "foundation/crypto/CryptoTypes.h"

#include "common/Error.h"

#include "crypto/AlgId.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Typed keys and signatures (docs/contracts/SIGNED_EXTENSIONS.md § Typed blobs): every key, signature, KEM ciphertext
 * and AEAD blob starts with its algorithm id (one byte, counted per kind, pp-cpp-crypto `crypto/AlgId.h`), then the
 * algorithm's bytes. JSON's base64 fields hold the typed blob; signed bytes hold `LenBytes(typed blob)`
 * (`SignedBytesWriter::Typed`). Identities (Account IDs, PeerIds, locker ids) hash the raw key, never the typed blob.
 *
 * This build writes ml-dsa-65 (signature kinds), ml-kem-768 (KEM kinds) and xchacha20-poly1305 (the AEAD blob).
 */

/** How a typed blob read: the receiver's order splits these (SIGNED_EXTENSIONS § The receiver's order). */
enum class TypedBlobStatus : uint8_t {
  Ok = 0,
  /** A nonzero id this build does not know for the blob's kind: update needed (`AppError::NewerFormat`). */
  UnknownAlgorithm = 1,
  /** Empty, not base64, id 0, or a known id with a body of the wrong size: refused as malformed. */
  Malformed = 2,
};

/** One typed blob read back: on `Ok`, its algorithm id and the raw bytes `MlDsa` / `HybridKem` take. */
struct TypedRead {
  TypedBlobStatus status = TypedBlobStatus::Malformed;
  uint8_t alg_id = 0;
  ByteVector raw;
  /** What was read and why it failed, for the log (empty on `Ok`). */
  std::string detail;

  bool ok() const { return status == TypedBlobStatus::Ok; }
  /**
   * The failure as an Error: `UnknownAlgorithm` is `AppError::NewerFormat` (`AppError::IsNewerFormat` is true);
   * `Malformed` a plain refusal (never NewerFormat). Only meaningful when `!ok()`.
   */
  Error ToError() const;
};

/** Read a typed blob of `kind`; `what` names the field for the log ("account card signing key"). */
TypedRead ReadTypedBlob(pp::BlobKind kind, std::span<const uint8_t> blob, std::string_view what);
/** The same from a JSON field's standard base64 (not base64 or empty: `Malformed`). */
TypedRead ReadTypedBlobB64(pp::BlobKind kind, const std::string& blob_b64, std::string_view what);

/** The algorithm id this build writes for `kind`. */
uint8_t WrittenAlgId(pp::BlobKind kind);

/**
 * `id ‖ raw` with this build's algorithm for `kind`. An error only when `raw` is not that algorithm's size (a bug in
 * the caller: keys and signatures from `MlDsa` / `HybridKem` always fit).
 */
Roe<ByteVector> WriteTypedBlob(pp::BlobKind kind, std::span<const uint8_t> raw);
/** `WriteTypedBlob`, in standard base64 for a JSON field. */
Roe<std::string> WriteTypedBlobB64(pp::BlobKind kind, std::span<const uint8_t> raw);

/** The four typed blobs signed objects carry: ML-DSA-65 key (1953 bytes) and signature (3310), ML-KEM-768 key (1185)
 * and ciphertext (1089). */
inline Roe<ByteVector> TypedSigningPublicKey(std::span<const uint8_t> raw) {
  return WriteTypedBlob(pp::BlobKind::kSignaturePublicKey, raw);
}
inline Roe<ByteVector> TypedSignature(std::span<const uint8_t> raw) {
  return WriteTypedBlob(pp::BlobKind::kSignature, raw);
}
inline Roe<ByteVector> TypedKemPublicKey(std::span<const uint8_t> raw) {
  return WriteTypedBlob(pp::BlobKind::kKemPublicKey, raw);
}
inline Roe<ByteVector> TypedKemCiphertext(std::span<const uint8_t> raw) {
  return WriteTypedBlob(pp::BlobKind::kKemCiphertext, raw);
}

/** Sign `message` with a raw ML-DSA-65 secret key: the typed signature. */
Roe<ByteVector> SignTyped(const ByteVector& secret_key, const ByteVector& message);

/**
 * Step 4 of the receiver's order: `signature` verifies `message` under `public_key`, both read `Ok` before (step 3).
 * A signature verifies only with a key of the same algorithm id. Any error is a forgery (a hard refusal).
 */
Roe<void> VerifyTyped(const TypedRead& public_key, const ByteVector& message, const TypedRead& signature);

} // namespace pbr
