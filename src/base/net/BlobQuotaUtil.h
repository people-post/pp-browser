#pragma once

#include "base/net/BlobClient.h"
#include "common/Error.h"

#include <string>
#include "common/PbrCompat.h"

namespace pbr {

struct BlobQuotaRecoveryPlan {
  RelayBlobRecord blob_to_delete;
  BlobUsageSummary usage;
};

bool IsBlobQuotaError(const Error& err);

/** Pure net helper — callers resolve relay_user_id (no IdentityStore). */
Roe<BlobQuotaRecoveryPlan> PlanOldestRelayBlobDeletion(IBlobClient& blob, const std::string& relay_user_id,
                                                       const std::string& protected_blob_id = "");

Roe<void> DeleteRelayBlob(IBlobClient& blob, const std::string& relay_user_id, const std::string& blob_id);

/** List remote blobs and delete the oldest deletable entry (R009). Local files are untouched. */
Roe<void> FreeOldestRelayBlobSlot(IBlobClient& blob, const std::string& relay_user_id,
                                  const std::string& protected_blob_id = "");

} // namespace pbr
