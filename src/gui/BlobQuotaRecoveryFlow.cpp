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
  // Used / total of the files relay, in bytes ("?" for a total it did not say).
  return Tr("blob.quota.usage_bytes",
            {{"used", FormatAttachmentByteSize(plan.file_usage.bytes_used)},
             {"total", plan.file_usage.quota_bytes > 0 ? FormatAttachmentByteSize(plan.file_usage.quota_bytes)
                                                       : std::string("?")}});
}

std::vector<std::string> BlobQuotaRecoveryFlow::Detail::IdsToDelete(const BlobQuotaRecoveryPlan& plan) {
  if (!plan.blob_ids_to_delete.empty()) {
    return plan.blob_ids_to_delete;
  }
  return {plan.blob_to_delete.blob_id};
}

Roe<void> BlobQuotaRecoveryFlow::Detail::FreeAll(const BlobQuotaFreeSlot& free_slot,
                                                 const std::vector<std::string>& blob_ids) {
  for (const std::string& blob_id : blob_ids) {
    if (auto freed = free_slot(blob_id); !freed) {
      return freed;
    }
  }
  return {};
}

void BlobQuotaRecoveryFlow::RunVoidUpload(
    std::function<Roe<void>()> upload, std::function<void(Roe<void>)> on_complete,
    std::function<Roe<BlobQuotaRecoveryPlan>(const Error& quota_error)> plan_recovery, BlobQuotaFreeSlot free_slot) {
  Detail::RunUploadImpl(std::move(upload), std::move(on_complete), std::move(plan_recovery), std::move(free_slot));
}

} // namespace pbr
