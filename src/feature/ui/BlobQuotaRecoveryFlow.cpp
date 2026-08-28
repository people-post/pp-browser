#include "feature/ui/BlobQuotaRecoveryFlow.h"
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
  return Tr("blob.quota.message", {{"detail", detail}});
}

void BlobQuotaRecoveryFlow::RunVoidUpload(std::function<Roe<void>()> upload,
                                            std::function<void(Roe<void>)> on_complete,
                                            std::function<Roe<BlobQuotaRecoveryPlan>()> plan_recovery,
                                            std::function<Roe<void>()> free_slot) {
  Detail::RunUploadImpl(std::move(upload), std::move(on_complete), std::move(plan_recovery), std::move(free_slot));
}

} // namespace pbr
