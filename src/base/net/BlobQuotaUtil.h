#pragma once

#include "base/net/BlobClient.h"
#include "common/Error.h"

#include <string>
#include "common/PbrCompat.h"

namespace pbr {

class IdentityStore;

struct BlobQuotaRecoveryPlan {
  RelayBlobRecord blob_to_delete;
  BlobUsageSummary usage;
};

bool IsBlobQuotaError(const Error& err);

Roe<BlobQuotaRecoveryPlan> PlanOldestRelayBlobDeletion(IBlobClient& blob, IdentityStore& identity,
                                                       const std::string& protected_blob_id = "");

Roe<void> DeleteRelayBlob(IBlobClient& blob, IdentityStore& identity, const std::string& blob_id);

/** List remote blobs and delete the oldest deletable entry (R009). Local files are untouched. */
Roe<void> FreeOldestRelayBlobSlot(IBlobClient& blob, IdentityStore& identity,
                                  const std::string& protected_blob_id = "");

} // namespace pbr
