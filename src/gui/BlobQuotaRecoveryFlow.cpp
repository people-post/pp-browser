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
  // Used / total of the tier that is full: the large tier counts bytes, the small tier files.
  const BlobUsageSummary& usage = plan.usage;
  const bool large_full = plan.full_tier == BlobTier::Large && usage.large_included_bytes > 0;
  const std::string usage_line =
      large_full ? Tr("blob.quota.usage_bytes", {{"used", FormatAttachmentByteSize(usage.large_bytes)},
                                                 {"total", FormatAttachmentByteSize(usage.large_included_bytes)}})
                 : Tr("blob.quota.usage_files", {{"used", std::to_string(usage.small_count)},
                                                 {"total", plan.small_max_objects > 0
                                                               ? std::to_string(plan.small_max_objects)
                                                               : std::string("?")}});
  return Tr("blob.quota.message", {{"usage", usage_line}, {"detail", detail}});
}

void BlobQuotaRecoveryFlow::RunVoidUpload(std::function<Roe<void>()> upload,
                                            std::function<void(Roe<void>)> on_complete,
                                            std::function<Roe<BlobQuotaRecoveryPlan>(const Error& quota_error)> plan_recovery,
                                            std::function<Roe<void>(const std::string& blob_id)> free_slot) {
  Detail::RunUploadImpl(std::move(upload), std::move(on_complete), std::move(plan_recovery), std::move(free_slot));
}

} // namespace pbr
