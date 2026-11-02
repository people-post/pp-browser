#pragma once

#include "lib/net/BlobClient.h"
#include "common/Error.h"

#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

struct BlobQuotaRecoveryPlan {
  /**
   * What the dialog names. Files tier: the oldest of the files to delete, with `byte_length` the bytes they
   * free together and `created_at` the oldest one's completion time.
   */
  RelayBlobRecord blob_to_delete;
  /** Every id the confirmed recovery deletes (one for the `/blobs` tiers; as many as free enough for Files). */
  std::vector<std::string> blob_ids_to_delete;
  BlobUsageSummary usage;
  /** The Files tier's "used / total" (bytes). */
  RelayFileUsage file_usage;
  /** The tier that is full: the 429's tier, else the tier of `blob_to_delete`. */
  BlobTier full_tier = BlobTier::Small;
  /** The small tier's file limit for the "used / total" line; the list answer has none (0: not known). */
  uint64_t small_max_objects = 0;
};

bool IsBlobQuotaError(const Error& err);

/** Which tier a quota error says is full (the relay's 429 text, or the files relay's code); none when unsaid. */
std::optional<BlobTier> BlobQuotaTierOf(const Error& err);

/** The size a refused `/files/start` asked for (HttpBlobClient notes it in the detail); 0 when not there. */
uint64_t RelayFileQuotaNeededBytes(const Error& err);

/**
 * The files relay is full (`QUOTA_FILE_BYTES`): from `/files/list` (complete files, the newest 500) the oldest by
 * completion, as many as it takes for `needed_bytes` to fit under the quota (at least one). None of them is
 * deleted here. When even all of them would not free enough, a QuotaExceeded error and no plan: nothing is
 * deleted for an upload that still could not go.
 */
Roe<BlobQuotaRecoveryPlan> PlanOldestRelayFileDeletion(IBlobClient& blob, const std::string& relay_user_id,
                                                       uint64_t needed_bytes);

/** `/files/delete` of one id (an attachment's relay copy). */
Roe<void> DeleteRelayFile(IBlobClient& blob, const std::string& relay_user_id, const std::string& blob_id);

/**
 * Pure net helper — callers resolve relay_user_id (no IdentityStore). The oldest retained blob that frees the
 * full tier (`full_tier`; none: any tier) and is not an icon or the protected blob.
 */
Roe<BlobQuotaRecoveryPlan> PlanOldestRelayBlobDeletion(IBlobClient& blob, const std::string& relay_user_id,
                                                       const std::string& protected_blob_id = "",
                                                       std::optional<BlobTier> full_tier = std::nullopt);

Roe<void> DeleteRelayBlob(IBlobClient& blob, const std::string& relay_user_id, const std::string& blob_id);

/** List remote blobs and delete the oldest deletable entry (R009). Local files are untouched. */
Roe<void> FreeOldestRelayBlobSlot(IBlobClient& blob, const std::string& relay_user_id,
                                  const std::string& protected_blob_id = "",
                                  std::optional<BlobTier> full_tier = std::nullopt);

} // namespace pbr
