#include "lib/broadcast/BroadcastSignedJson.h"

#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/SignedReceive.h"
#include "foundation/error/AppError.h"

#include "common/PbrCompat.h"

namespace pbr {

Roe<int> ReadSchemaVersion(const Object& json, const std::string_view what, const int only) {
  const std::optional<int64_t> version = OutsideVersionOfJson(json, "schema_version");
  if (!version) {
    return Error(std::string(what) + ": no schema_version (made by an older app)");
  }
  if (*version < only) {
    return Error(std::string(what) + ": schema_version " + std::to_string(*version) + " is made by an older app");
  }
  if (*version > only) {
    return AppError::NewerFormat(std::string(what) + ": schema_version " + std::to_string(*version) +
                                 " is made by a newer app");
  }
  return static_cast<int>(*version);
}

Roe<std::string> ReadOptionalString(const Object& json, const std::string& key, const std::string_view what) {
  if (!json.contains(key)) {
    return std::string();
  }
  auto value = json.getString(key);
  if (!value) {
    return Error(std::string(what) + ": " + key + " is not a string");
  }
  return *value;
}

Roe<int64_t> ReadOptionalInt(const Object& json, const std::string& key, const int64_t fallback,
                             const std::string_view what) {
  if (!json.contains(key)) {
    return fallback;
  }
  auto value = json.getIf<int64_t>(key);
  if (!value) {
    return Error(std::string(what) + ": " + key + " is not an integer");
  }
  return *value;
}

Roe<ByteVector> ReadOptionalBase64(const std::string& b64, const std::string_view what) {
  if (b64.empty()) {
    return ByteVector{};
  }
  auto bytes = Base64Decode(b64);
  if (!bytes) {
    return Error(std::string(what) + " is not base64");
  }
  return std::move(*bytes);
}

TypedRead PublisherSigningKey(const ByteVector& raw_public_key) {
  auto typed = TypedSigningPublicKey(raw_public_key);
  if (!typed) {
    TypedRead refused;
    refused.detail = "publisher key: not an ML-DSA-65 public key";
    return refused;
  }
  return ReadTypedBlob(pp::BlobKind::kSignaturePublicKey, *typed, "publisher key");
}

} // namespace pbr
