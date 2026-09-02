#include "domain/net/BlobClient.h"

#include "common/Utilities.h"
#include "common/PbrCompat.h"

namespace pbr {

std::string BlobPurposeToWire(const BlobPurpose purpose) {
  return purpose == BlobPurpose::Icon ? "icon" : "file";
}

std::optional<BlobPurpose> BlobPurposeFromWire(const std::string& value) {
  if (value == "icon") {
    return BlobPurpose::Icon;
  }
  if (value == "file") {
    return BlobPurpose::File;
  }
  return std::nullopt;
}

std::optional<BlobTier> BlobTierFromWire(const std::string& value) {
  if (value == "small") {
    return BlobTier::Small;
  }
  if (value == "large") {
    return BlobTier::Large;
  }
  return std::nullopt;
}

std::optional<BlobStatus> BlobStatusFromWire(const std::string& value) {
  if (value == "pending") {
    return BlobStatus::Pending;
  }
  if (value == "retained") {
    return BlobStatus::Retained;
  }
  return std::nullopt;
}

Roe<BlobPresignResult> UploadRelayBlobBytes(IBlobClient& blob, const std::string& relay_user_id,
                                            const std::string& content_type, const BlobPurpose purpose,
                                            const std::string& body) {
  auto presign = blob.Presign(relay_user_id, content_type, body.size(), purpose);
  if (!presign) {
    return presign.error();
  }
  auto put = blob.PutUpload(presign.value().upload_url, content_type, body);
  if (!put) {
    return put.error();
  }
  auto retain = blob.Retain(relay_user_id, presign.value().blob_id);
  if (!retain) {
    return retain.error();
  }
  return presign.value();
}

} // namespace pbr
