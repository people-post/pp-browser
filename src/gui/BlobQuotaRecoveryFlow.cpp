#include "gui/BlobQuotaRecoveryFlow.h"
#include "common/PbrCompat.h"

namespace pbr {

namespace {

std::string BlobPurposeLabel(const BlobPurpose purpose) {
  return purpose == BlobPurpose::Icon ? Tr("blob.quota.purpose_icon") : Tr("blob.quota.purpose_file");
}

} // namespace

std::string BlobQuotaRecoveryFlow::Detail::BuildQuotaConfirmMessage(const BlobQuotaRecoveryPlan& plan) {
  const RelayBlobRecord& blob = plan.blob_to_delete;
  const std::string detail =
      Tr("blob.quota.detail", {{"purpose", BlobPurposeLabel(blob.purpose)},
                               {"size", FormatAttachmentByteSize(blob.byte_length)},
                               {"date", blob.created_at.empty() ? Tr("blob.quota.date_unknown") : blob.created_at}});
  return Tr("blob.quota.message", {{"usage", UsageLine(plan)}, {"detail", detail}});
}

std::string BlobQuotaRecoveryFlow::Detail::UsageLine(const BlobQuotaRecoveryPlan& plan) {
  // Used / total of the tier that is full: the files relay and the large tier count bytes, the small tier files.
  if (plan.full_tier == BlobTier::Files && plan.file_usage.quota_bytes > 0) {
    return Tr("blob.quota.usage_bytes", {{"used", FormatAttachmentByteSize(plan.file_usage.bytes_used)},
                                         {"total", FormatAttachmentByteSize(plan.file_usage.quota_bytes)}});
  }
  const BlobUsageSummary& usage = plan.usage;
  const bool large_full = plan.full_tier == BlobTier::Large && usage.large_included_bytes > 0;
  return large_full
             ? Tr("blob.quota.usage_bytes", {{"used", FormatAttachmentByteSize(usage.large_bytes)},
                                             {"total", FormatAttachmentByteSize(usage.large_included_bytes)}})
             : Tr("blob.quota.usage_files",
                  {{"used", std::to_string(usage.small_count)},
                   {"total", plan.small_max_objects > 0 ? std::to_string(plan.small_max_objects) : std::string("?")}});
}

std::vector<std::string> BlobQuotaRecoveryFlow::Detail::IdsToDelete(const BlobQuotaRecoveryPlan& plan) {
  if (!plan.blob_ids_to_delete.empty()) {
    return plan.blob_ids_to_delete;
  }
  return {plan.blob_to_delete.blob_id};
}

Roe<void> BlobQuotaRecoveryFlow::Detail::FreeAll(const std::function<Roe<void>(const std::string& blob_id)>& free_slot,
                                                 const std::vector<std::string>& blob_ids) {
  for (const std::string& blob_id : blob_ids) {
    if (auto freed = free_slot(blob_id); !freed) {
      return freed;
    }
  }
  return {};
}

void BlobQuotaRecoveryFlow::RunVoidUpload(std::function<Roe<void>()> upload,
                                            std::function<void(Roe<void>)> on_complete,
                                            std::function<Roe<BlobQuotaRecoveryPlan>(const Error& quota_error)> plan_recovery,
                                            std::function<Roe<void>(const std::string& blob_id)> free_slot) {
  Detail::RunUploadImpl(std::move(upload), std::move(on_complete), std::move(plan_recovery), std::move(free_slot));
}

} // namespace pbr
