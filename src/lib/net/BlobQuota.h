#pragma once

#include "lib/net/BlobClient.h"
#include "common/Error.h"

#include <optional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

struct BlobQuotaRecoveryPlan {
  RelayBlobRecord blob_to_delete;
  BlobUsageSummary usage;
  /** The tier that is full: the 429 text's tier, else the tier of `blob_to_delete`. */
  BlobTier full_tier = BlobTier::Small;
  /** The small tier's file limit for the "used / total" line; the list answer has none (0: not known). */
  uint64_t small_max_objects = 0;
};

bool IsBlobQuotaError(const Error& err);

/** Which tier a quota error says is full (from the relay's 429 text); none when it does not say. */
std::optional<BlobTier> BlobQuotaTierOf(const Error& err);

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
