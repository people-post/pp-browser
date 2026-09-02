#include "domain/net/BlobQuotaUtil.h"

#include "foundation/error/AppError.h"
#include "common/PbrCompat.h"

#include <optional>

namespace pbr {

namespace {

Roe<std::string> RequireRelayUserId(const std::string& relay_user_id) {
  if (relay_user_id.empty()) {
    return Error("Register on the network before using relay blob storage");
  }
  return relay_user_id;
}

} // namespace

bool IsBlobQuotaError(const Error& err) {
  return AppError::CategoryOf(err) == ErrorCategory::Blob &&
         err.code == static_cast<int32_t>(Err::Blob::QuotaExceeded);
}

Roe<BlobQuotaRecoveryPlan> PlanOldestRelayBlobDeletion(IBlobClient& blob, const std::string& relay_user_id,
                                                       const std::string& protected_blob_id) {
  auto uid = RequireRelayUserId(relay_user_id);
  if (!uid) {
    return uid.error();
  }

  auto listed = blob.List(*uid);
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

Roe<void> DeleteRelayBlob(IBlobClient& blob, const std::string& relay_user_id, const std::string& blob_id) {
  if (blob_id.empty()) {
    return Error("blob_id is required");
  }
  auto uid = RequireRelayUserId(relay_user_id);
  if (!uid) {
    return uid.error();
  }
  return blob.Delete(*uid, blob_id);
}

Roe<void> FreeOldestRelayBlobSlot(IBlobClient& blob, const std::string& relay_user_id,
                                  const std::string& protected_blob_id) {
  auto plan = PlanOldestRelayBlobDeletion(blob, relay_user_id, protected_blob_id);
  if (!plan) {
    return plan.error();
  }
  return DeleteRelayBlob(blob, relay_user_id, plan->blob_to_delete.blob_id);
}

} // namespace pbr
