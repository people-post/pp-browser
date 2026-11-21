#include "lib/delivery/TypedSigning.h"

#include "foundation/crypto/CryptoUtil.h"

#include "common/PbrCompat.h"

namespace pbr {

Roe<std::string> SignTypedB64(const RawSignFn& sign, const std::vector<uint8_t>& bytes) {
  if (!sign) {
    return Error("No signer");
  }
  auto raw_b64 = sign(bytes);
  if (!raw_b64) {
    return raw_b64.error();
  }
  auto raw = Base64Decode(*raw_b64);
  if (!raw) {
    return raw.error();
  }
  auto typed = TypedSignature(*raw);
  if (!typed) {
    return typed.error();
  }
  return Base64Encode(*typed);
}

TypedRead ReadHeldSigningKeyB64(const std::string& key_b64, const std::string_view what) {
  auto raw = Base64Decode(key_b64);
  if (!raw) {
    TypedRead read;
    read.status = TypedBlobStatus::Malformed;
    read.detail = std::string(what) + ": not base64";
    return read;
  }
  auto typed = TypedSigningPublicKey(*raw); // the size of an ML-DSA-65 key, else malformed
  if (!typed) {
    TypedRead read;
    read.status = TypedBlobStatus::Malformed;
    read.detail = std::string(what) + ": not an ML-DSA-65 key";
    return read;
  }
  return ReadTypedBlob(pp::BlobKind::kSignaturePublicKey, *typed, what);
}

} // namespace pbr
