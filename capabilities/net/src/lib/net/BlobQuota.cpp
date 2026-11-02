#include "lib/net/BlobQuota.h"

#include "foundation/error/AppError.h"
#include "common/PbrCompat.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string_view>
#include <vector>

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

std::optional<BlobTier> BlobQuotaTierOf(const Error& err) {
  if (RelayFileErrorCode(err) == "QUOTA_FILE_BYTES") {
    return BlobTier::Files;
  }
  if (err.message.find("Small file quota") != std::string::npos) {
    return BlobTier::Small;
  }
  if (err.message.find("Large file storage quota") != std::string::npos) {
    return BlobTier::Large;
  }
  return std::nullopt;
}

Roe<BlobQuotaRecoveryPlan> PlanOldestRelayBlobDeletion(IBlobClient& blob, const std::string& relay_user_id,
                                                       const std::string& protected_blob_id,
                                                       const std::optional<BlobTier> full_tier) {
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
    // Never a profile photo, never an upload still in flight, and only the tier that is full.
    if (record.purpose == BlobPurpose::Icon || record.status != BlobStatus::Retained) {
      continue;
    }
    if (full_tier && record.tier != *full_tier) {
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
  plan.blob_ids_to_delete = {oldest->blob_id};
  plan.usage = listed->usage;
  plan.full_tier = full_tier.value_or(oldest->tier);
  return plan;
}

uint64_t RelayFileQuotaNeededBytes(const Error& err) {
  constexpr std::string_view kKey = "byte_length=";
  const size_t at = err.message.rfind(kKey);
  if (at == std::string::npos) {
    return 0;
  }
  uint64_t value = 0;
  for (size_t i = at + kKey.size(); i < err.message.size() && std::isdigit(static_cast<unsigned char>(err.message[i]));
       ++i) {
    value = value * 10 + static_cast<uint64_t>(err.message[i] - '0');
  }
  return value;
}

Roe<BlobQuotaRecoveryPlan> PlanOldestRelayFileDeletion(IBlobClient& blob, const std::string& relay_user_id,
                                                       const uint64_t needed_bytes) {
  auto uid = RequireRelayUserId(relay_user_id);
  if (!uid) {
    return uid.error();
  }
  auto listed = blob.FileList(*uid, "complete");
  if (!listed) {
    return listed.error();
  }
  std::vector<RelayFileRecord> files;
  for (const RelayFileRecord& file : listed->files) {
    if (file.status == "complete" && !file.blob_id.empty()) {
      files.push_back(file);
    }
  }
  if (files.empty()) {
    return AppError::Blob(Err::Blob::NothingToDelete, "No relay files to remove");
  }
  const auto completed = [](const RelayFileRecord& file) {
    return file.completed_at.empty() ? file.created_at : file.completed_at;
  };
  std::stable_sort(files.begin(), files.end(),
                   [&](const RelayFileRecord& a, const RelayFileRecord& b) { return completed(a) < completed(b); });

  const RelayFileUsage& usage = listed->usage;
  const auto fits = [&](const uint64_t freed) {
    // Quota unknown (0): one file is all the plan can promise.
    if (usage.quota_bytes == 0) {
      return freed > 0;
    }
    const uint64_t used = usage.bytes_used > freed ? usage.bytes_used - freed : 0;
    return used + needed_bytes <= usage.quota_bytes;
  };
  BlobQuotaRecoveryPlan plan;
  uint64_t freed = 0;
  for (const RelayFileRecord& file : files) {
    plan.blob_ids_to_delete.push_back(file.blob_id);
    freed += file.byte_length;
    if (fits(freed)) {
      break;
    }
  }
  if (!fits(freed)) {
    return AppError::Blob(Err::Blob::QuotaExceeded, "Relay file storage stays full without every listed file");
  }
  plan.blob_to_delete.blob_id = files.front().blob_id;
  plan.blob_to_delete.byte_length = freed;
  plan.blob_to_delete.created_at = completed(files.front());
  plan.blob_to_delete.purpose = BlobPurpose::File;
  plan.blob_to_delete.status = BlobStatus::Retained;
  plan.blob_to_delete.tier = BlobTier::Files;
  plan.file_usage = usage;
  plan.full_tier = BlobTier::Files;
  return plan;
}

Roe<void> DeleteRelayFile(IBlobClient& blob, const std::string& relay_user_id, const std::string& blob_id) {
  if (blob_id.empty()) {
    return Error("blob_id is required");
  }
  auto uid = RequireRelayUserId(relay_user_id);
  if (!uid) {
    return uid.error();
  }
  return blob.FileDelete(*uid, blob_id);
}

Roe<void> DeleteRelayQuotaItem(IBlobClient& blob, const std::string& relay_user_id, const std::string& blob_id,
                               const BlobTier tier) {
  return tier == BlobTier::Files ? DeleteRelayFile(blob, relay_user_id, blob_id)
                                 : DeleteRelayBlob(blob, relay_user_id, blob_id);
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
                                  const std::string& protected_blob_id, const std::optional<BlobTier> full_tier) {
  auto plan = PlanOldestRelayBlobDeletion(blob, relay_user_id, protected_blob_id, full_tier);
  if (!plan) {
    return plan.error();
  }
  return DeleteRelayBlob(blob, relay_user_id, plan->blob_to_delete.blob_id);
}

} // namespace pbr
