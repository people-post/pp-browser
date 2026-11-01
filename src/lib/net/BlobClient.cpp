#include "lib/net/BlobClient.h"

#include "common/Utilities.h"
#include "common/PbrCompat.h"

#include <fstream>
#include <iterator>

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

Roe<void> IBlobClient::PutUploadFile(const std::string& upload_url, const std::string& content_type,
                                     const std::string& path,
                                     const std::function<bool(uint64_t, uint64_t)>& progress) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return Error("Upload file is not readable");
  }
  const std::string body((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (progress && !progress(0, body.size())) {
    return Error("Upload cancelled");
  }
  auto put = PutUpload(upload_url, content_type, body);
  if (put && progress) {
    (void)progress(body.size(), body.size());
  }
  return put;
}

Roe<BlobPresignResult> UploadRelayBlobFile(IBlobClient& blob, const std::string& relay_user_id,
                                           const std::string& content_type, const BlobPurpose purpose,
                                           const std::string& path, const uint64_t byte_length,
                                           const std::function<bool(uint64_t, uint64_t)>& progress) {
  auto presign = blob.Presign(relay_user_id, content_type, byte_length, purpose);
  if (!presign) {
    return presign.error();
  }
  auto put = blob.PutUploadFile(presign.value().upload_url, content_type, path, progress);
  if (!put) {
    return put.error();
  }
  auto retain = blob.Retain(relay_user_id, presign.value().blob_id);
  if (!retain) {
    return retain.error();
  }
  return presign.value();
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
