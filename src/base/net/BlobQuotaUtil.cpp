#include "base/net/BlobQuotaUtil.h"

#include "base/people/IdentityStore.h"
#include "base/error/AppError.h"

namespace pbr {

namespace {

Roe<std::string> RequireRegisteredRelayUserId(IdentityStore& identity) {
  auto loaded = identity.Get();
  if (!loaded) {
    return loaded.error();
  }
  if (!loaded->registered || loaded->relay_user_id.empty()) {
    return Error("Register on the network before using relay blob storage");
  }
  return loaded->relay_user_id;
}

} // namespace

bool IsBlobQuotaError(const Error& err) {
  return AppError::CategoryOf(err) == ErrorCategory::Blob &&
         err.code == static_cast<int32_t>(Err::Blob::QuotaExceeded);
}

Roe<BlobQuotaRecoveryPlan> PlanOldestRelayBlobDeletion(IBlobClient& blob, IdentityStore& identity,
                                                       const std::string& protected_blob_id) {
  auto relay_user_id = RequireRegisteredRelayUserId(identity);
  if (!relay_user_id) {
    return relay_user_id.error();
  }

  auto listed = blob.List(*relay_user_id);
  if (!listed) {
    return listed.error();
  }
  if (listed->blobs.empty()) {
    return AppError::Blob(Err::Blob::NothingToDelete, "No relay blobs to remove");
  }

  std::optional<RelayBlobRecord> oldest;
  for (const RelayBlobRecord& record : listed->blobs) {
    if (!protected_blob_id.empty() && record.blob_id == protected_blob_id) {
      continue;
    }
    if (!oldest || record.created_at < oldest->created_at) {
      oldest = record;
    }
  }
  if (!oldest) {
    return AppError::Blob(Err::Blob::NothingToDelete, "No deletable relay blobs");
  }

  BlobQuotaRecoveryPlan plan;
  plan.blob_to_delete = *oldest;
  plan.usage = listed->usage;
  return plan;
}

Roe<void> DeleteRelayBlob(IBlobClient& blob, IdentityStore& identity, const std::string& blob_id) {
  if (blob_id.empty()) {
    return Error("blob_id is required");
  }
  auto relay_user_id = RequireRegisteredRelayUserId(identity);
  if (!relay_user_id) {
    return relay_user_id.error();
  }
  return blob.Delete(*relay_user_id, blob_id);
}

Roe<void> FreeOldestRelayBlobSlot(IBlobClient& blob, IdentityStore& identity,
                                  const std::string& protected_blob_id) {
  auto plan = PlanOldestRelayBlobDeletion(blob, identity, protected_blob_id);
  if (!plan) {
    return plan.error();
  }
  return DeleteRelayBlob(blob, identity, plan->blob_to_delete.blob_id);
}

} // namespace pbr
