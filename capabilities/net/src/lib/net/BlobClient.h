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

enum class BlobStatus { Pending, Retained };

struct BlobPresignResult {
  std::string blob_id;
  std::string upload_url;
  std::string public_url;
  std::string pending_expires_at;
};

struct RelayBlobRecord {
  std::string blob_id;
  std::string url;
  BlobStatus status = BlobStatus::Pending;
  std::string content_type;
  uint64_t byte_length = 0;
  BlobPurpose purpose = BlobPurpose::File;
  std::string created_at;
  std::string retained_at;
  std::string pending_expires_at;
};

/**
 * `/files/start`: where to PUT the ciphertext. `mode` "single" (one PUT to `upload_url`) or "multipart"
 * (`part_count` parts, each exactly `part_bytes` but the last, each PUT to a URL from `/files/part`).
 */
struct RelayFileStartResult {
  std::string blob_id;
  std::string mode;
  std::string upload_url;
  uint64_t part_bytes = 0;
  uint64_t part_count = 0;
  int64_t expires_in = 0;
  std::string pending_expires_at;
};

/** `/files/part`: the PUT URL of one part and that part's size (the last part's is the remainder). */
struct RelayFilePartResult {
  std::string upload_url;
  uint64_t part_bytes = 0;
  int64_t expires_in = 0;
};

/**
 * `/files/complete`: `share_path` is `/api/relay/v1/files/<blob_id>/download?t=<token>`, `share_url` the relay
 * origin plus it. The token is the download credential and is returned once: never log either.
 */
struct RelayFileCompleteResult {
  std::string blob_id;
  std::string share_path;
  std::string share_url;
  std::string expires_at;
};

/** One `/files/list` entry; times are the relay's ISO-8601 UTC strings (empty when not set). */
struct RelayFileRecord {
  std::string blob_id;
  std::string status;
  uint64_t byte_length = 0;
  std::string created_at;
  std::string completed_at;
  std::string expires_at;
  std::string pending_expires_at;
};

struct RelayFileUsage {
  uint64_t bytes_used = 0;
  uint64_t quota_bytes = 0;
  uint64_t pending_count = 0;
  uint64_t max_pending = 0;
};

/** Newest first, at most 500 entries (the relay's cap). */
struct RelayFileListResult {
  std::vector<RelayFileRecord> files;
  RelayFileUsage usage;
};

/** A finished upload: the id (for delete) and the share link the message carries. */
struct RelayFileUpload {
  std::string blob_id;
  std::string url;
};

std::string BlobPurposeToWire(BlobPurpose purpose);
std::optional<BlobPurpose> BlobPurposeFromWire(const std::string& value);
std::optional<BlobStatus> BlobStatusFromWire(const std::string& value);

class IBlobClient {
public:
  virtual ~IBlobClient() = default;

  virtual Roe<BlobPresignResult> Presign(const std::string& relay_user_id, const std::string& content_type,
                                         uint64_t byte_length, BlobPurpose purpose) = 0;
  virtual Roe<void> Retain(const std::string& relay_user_id, const std::string& blob_id) = 0;
  virtual Roe<void> Delete(const std::string& relay_user_id, const std::string& blob_id) = 0;
  virtual Roe<void> SetProfileIcon(const std::string& relay_user_id, const std::string& url, const std::string& blob_id,
                                   const std::string& kind) = 0;
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

  /**
   * PUT `length` bytes of the file from `offset` (one multipart part), read from disk as they go out.
   * `progress(done, total)` counts within this range; false cancels. The default reads the range and calls
   * PutUpload (test doubles).
   */
  virtual Roe<void> PutUploadFileRange(const std::string& upload_url, const std::string& content_type,
                                       const std::string& path, uint64_t offset, uint64_t length,
                                       const std::function<bool(uint64_t done, uint64_t total)>& progress);

  // The encrypted-file relay (`/files/*`), for chat attachments; the `/blobs` calls above stay for profile photos.
  virtual Roe<RelayFileStartResult> FileStart(const std::string& relay_user_id, uint64_t byte_length) = 0;
  /** A fresh PUT URL for part `part_number` (1-based) of a multipart upload. */
  virtual Roe<RelayFilePartResult> FilePart(const std::string& relay_user_id, const std::string& blob_id,
                                            uint64_t part_number) = 0;
  virtual Roe<RelayFileCompleteResult> FileComplete(const std::string& relay_user_id, const std::string& blob_id) = 0;
  virtual Roe<void> FileDelete(const std::string& relay_user_id, const std::string& blob_id) = 0;
  /** `status_filter` "" (pending and complete), "pending" or "complete". */
  virtual Roe<RelayFileListResult> FileList(const std::string& relay_user_id, const std::string& status_filter) = 0;
};

/** The relay error code (`QUOTA_FILE_BYTES`, `BLOB_NOT_PENDING`, ...) a failed `/files` call carried, or "". */
std::string RelayFileErrorCode(const Error& err);

/**
 * files/start → PUT the staged ciphertext (`cipher_bytes` long: the size the relay reserves and signs into the
 * PUT) → files/complete. A `multipart` answer (over the relay's part size, 16 MiB) is sent part by part in order,
 * each part from the offset the relay's `part_bytes` gives; a part that fails on the network, with a 5xx or a 403
 * (expired URL) is tried again with a fresh `/files/part` URL at most twice, and so is complete. A cancel
 * (`progress` false) or any other refusal stops at once. Once start has answered, any later
 * failure deletes that blob id (best effort) and returns the original error, so no pending upload is left on the
 * account. A start refused for too many pending uploads first deletes this account's pending uploads started over
 * an hour ago (never newer ones: another device may still be uploading) and starts once more.
 */
Roe<RelayFileUpload> UploadRelayFile(IBlobClient& blob, const std::string& relay_user_id, const std::string& path,
                                     uint64_t cipher_bytes,
                                     const std::function<bool(uint64_t done, uint64_t total)>& progress);

/** The same for ciphertext held in memory (single PUT only: a `multipart` answer is an error). */
Roe<RelayFileUpload> UploadRelayFileBytes(IBlobClient& blob, const std::string& relay_user_id, const std::string& body);

/** presign → PUT → retain; returns public URL metadata. */
Roe<BlobPresignResult> UploadRelayBlobBytes(IBlobClient& blob, const std::string& relay_user_id,
                                            const std::string& content_type, BlobPurpose purpose,
                                            const std::string& body);

} // namespace pbr
