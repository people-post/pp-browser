#pragma once

#include "common/Error.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

enum class BlobPurpose { Icon, File };

enum class BlobTier { Small, Large };

enum class BlobStatus { Pending, Retained };

struct BlobPresignResult {
  std::string blob_id;
  std::string upload_url;
  std::string public_url;
  BlobTier tier = BlobTier::Small;
  std::string pending_expires_at;
};

struct RelayBlobRecord {
  std::string blob_id;
  std::string url;
  BlobStatus status = BlobStatus::Pending;
  BlobTier tier = BlobTier::Small;
  std::string content_type;
  uint64_t byte_length = 0;
  BlobPurpose purpose = BlobPurpose::File;
  std::string created_at;
  std::string retained_at;
  std::string pending_expires_at;
};

struct BlobUsageSummary {
  uint64_t small_count = 0;
  uint64_t large_bytes = 0;
  uint64_t large_included_bytes = 0;
  uint64_t overage_bytes = 0;
};

struct BlobListResult {
  std::vector<RelayBlobRecord> blobs;
  BlobUsageSummary usage;
};

std::string BlobPurposeToWire(BlobPurpose purpose);
std::optional<BlobPurpose> BlobPurposeFromWire(const std::string& value);
std::optional<BlobTier> BlobTierFromWire(const std::string& value);
std::optional<BlobStatus> BlobStatusFromWire(const std::string& value);

class IBlobClient {
public:
  virtual ~IBlobClient() = default;

  virtual Roe<BlobPresignResult> Presign(const std::string& relay_user_id, const std::string& content_type,
                                         uint64_t byte_length, BlobPurpose purpose) = 0;
  virtual Roe<void> Retain(const std::string& relay_user_id, const std::string& blob_id) = 0;
  virtual Roe<void> Delete(const std::string& relay_user_id, const std::string& blob_id) = 0;
  virtual Roe<BlobListResult> List(const std::string& relay_user_id, const std::string& status_filter = "") = 0;
  virtual Roe<void> SetProfileIcon(const std::string& relay_user_id, const std::string& url,
                                   const std::string& blob_id, const std::string& kind) = 0;
  virtual Roe<void> PutUpload(const std::string& upload_url, const std::string& content_type,
                              const std::string& body) = 0;
  /**
   * PUT the bytes of a file, read from disk as they go out (a large attachment is never held whole in memory
   * for this). `progress(done, total)` is called as bytes are sent; false cancels. The default reads the file
   * and calls PutUpload (test doubles).
   */
  virtual Roe<void> PutUploadFile(const std::string& upload_url, const std::string& content_type,
                                  const std::string& path,
                                  const std::function<bool(uint64_t done, uint64_t total)>& progress);
};

/** presign → PUT (from a file, with progress) → retain. */
Roe<BlobPresignResult> UploadRelayBlobFile(IBlobClient& blob, const std::string& relay_user_id,
                                           const std::string& content_type, BlobPurpose purpose,
                                           const std::string& path, uint64_t byte_length,
                                           const std::function<bool(uint64_t done, uint64_t total)>& progress);

/** presign → PUT → retain; returns public URL metadata. */
Roe<BlobPresignResult> UploadRelayBlobBytes(IBlobClient& blob, const std::string& relay_user_id,
                                            const std::string& content_type, BlobPurpose purpose,
                                            const std::string& body);

} // namespace pbr
