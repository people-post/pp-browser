#pragma once

#include "lib/net/BlobQuota.h"
#include "foundation/runtime/AppRuntime.h"
#include "common/Error.h"
#include "gui/UserFeedback.h"

#include "foundation/i18n/LocalizationService.h"
#include "lib/messaging/AttachmentCache.h"

#include <functional>
#include <utility>
#include "common/PbrCompat.h"

namespace pbr {

/** UI orchestration for R009: confirm, delete oldest remote blob, retry upload. */
struct BlobQuotaRecoveryFlow {
  static void RunVoidUpload(std::function<Roe<void>()> upload, std::function<void(Roe<void>)> on_complete,
                              std::function<Roe<BlobQuotaRecoveryPlan>(const Error& quota_error)> plan_recovery,
                              std::function<Roe<void>(const std::string& blob_id)> free_slot);

  template <typename T>
  static void RunUpload(std::function<Roe<T>()> upload, std::function<void(Roe<T>)> on_complete,
                        std::function<Roe<BlobQuotaRecoveryPlan>(const Error& quota_error)> plan_recovery,
                        std::function<Roe<void>(const std::string& blob_id)> free_slot) {
    Detail::RunUploadImpl(std::move(upload), std::move(on_complete), std::move(plan_recovery), std::move(free_slot));
  }

  /** Prefer for Amp peer-direct uploads — does not park a worker on PushChatBlob. */
  template <typename T>
  static void RunUploadAsync(std::function<void(std::function<void(Roe<T>)>)> upload,
                             std::function<void(Roe<T>)> on_complete,
                             std::function<Roe<BlobQuotaRecoveryPlan>(const Error& quota_error)> plan_recovery,
                             std::function<Roe<void>(const std::string& blob_id)> free_slot) {
    Detail::RunUploadAsyncImpl(std::move(upload), std::move(on_complete), std::move(plan_recovery),
                               std::move(free_slot));
  }

private:
  struct Detail {
    static std::string BuildQuotaConfirmMessage(const BlobQuotaRecoveryPlan& plan);

    template <typename T>
    static void FinishUpload(std::function<void(Roe<T>)> on_complete, Roe<T> result) {
      AppRuntime::PostUI([on_complete = std::move(on_complete), result = std::move(result)]() mutable {
        on_complete(std::move(result));
      });
    }

    template <typename T>
    static void PromptQuotaRecoveryAsync(std::function<void(std::function<void(Roe<T>)>)> upload,
                                         std::function<void(Roe<T>)> on_complete,
                                         std::function<Roe<void>(const std::string& blob_id)> free_slot, Roe<BlobQuotaRecoveryPlan> plan) {
      AppRuntime::PostUI([upload = std::move(upload), on_complete = std::move(on_complete),
                          free_slot = std::move(free_slot), plan = std::move(plan)]() mutable {
        if (!plan) {
          FinishUpload(on_complete, Roe<T>{plan.error()});
          return;
        }

        UserFeedback::Confirm(
            Tr("blob.quota.title"), BuildQuotaConfirmMessage(*plan),
            // The blob named in the dialog is the one deleted, not whatever is oldest by then.
            [upload = std::move(upload), on_complete = std::move(on_complete), free_slot = std::move(free_slot),
             blob_id = plan->blob_to_delete.blob_id](const bool confirmed) mutable {
              if (!confirmed) {
                FinishUpload(on_complete, Roe<T>{Error(Tr("blob.quota.cancelled"))});
                return;
              }
              AppRuntime::PostWorkerNormal([upload = std::move(upload), on_complete = std::move(on_complete),
                                            free_slot = std::move(free_slot), blob_id = std::move(blob_id)]() mutable {
                auto freed = free_slot(blob_id);
                if (!freed) {
                  FinishUpload(on_complete, Roe<T>{freed.error()});
                  return;
                }
                upload([on_complete = std::move(on_complete)](Roe<T> result) mutable {
                  FinishUpload(std::move(on_complete), std::move(result));
                });
              });
            },
            Tr("blob.quota.confirm"));
      });
    }

    template <typename T>
    static void BeginUploadAttemptAsync(std::function<void(std::function<void(Roe<T>)>)> upload,
                                        std::function<void(Roe<T>)> on_complete,
                                        std::function<Roe<BlobQuotaRecoveryPlan>(const Error& quota_error)> plan_recovery,
                                        std::function<Roe<void>(const std::string& blob_id)> free_slot) {
      AppRuntime::PostWorkerNormal([upload = std::move(upload), on_complete = std::move(on_complete),
                                    plan_recovery = std::move(plan_recovery),
                                    free_slot = std::move(free_slot)]() mutable {
        upload([upload, on_complete = std::move(on_complete), plan_recovery = std::move(plan_recovery),
                free_slot = std::move(free_slot)](Roe<T> result) mutable {
          if (result) {
            FinishUpload(std::move(on_complete), std::move(result));
            return;
          }
          if (!IsBlobQuotaError(result.error())) {
            FinishUpload(std::move(on_complete), Roe<T>{result.error()});
            return;
          }
          AppRuntime::PostWorkerNormal([upload = std::move(upload), on_complete = std::move(on_complete),
                                        plan_recovery = std::move(plan_recovery), free_slot = std::move(free_slot),
                                        quota_error = result.error()]() mutable {
            PromptQuotaRecoveryAsync(std::move(upload), std::move(on_complete), std::move(free_slot),
                                     plan_recovery(quota_error));
          });
        });
      });
    }

    template <typename T>
    static void PromptQuotaRecovery(std::function<Roe<T>()> upload, std::function<void(Roe<T>)> on_complete,
                                    std::function<Roe<void>(const std::string& blob_id)> free_slot, Roe<BlobQuotaRecoveryPlan> plan) {
      AppRuntime::PostUI([upload = std::move(upload), on_complete = std::move(on_complete),
                          free_slot = std::move(free_slot), plan = std::move(plan)]() mutable {
        if (!plan) {
          FinishUpload(on_complete, Roe<T>{plan.error()});
          return;
        }

        UserFeedback::Confirm(
            Tr("blob.quota.title"), BuildQuotaConfirmMessage(*plan),
            // The blob named in the dialog is the one deleted, not whatever is oldest by then.
            [upload = std::move(upload), on_complete = std::move(on_complete), free_slot = std::move(free_slot),
             blob_id = plan->blob_to_delete.blob_id](const bool confirmed) mutable {
              if (!confirmed) {
                FinishUpload(on_complete, Roe<T>{Error(Tr("blob.quota.cancelled"))});
                return;
              }
              AppRuntime::PostWorkerNormal([upload = std::move(upload), on_complete = std::move(on_complete),
                                            free_slot = std::move(free_slot), blob_id = std::move(blob_id)]() mutable {
                auto freed = free_slot(blob_id);
                if (!freed) {
                  FinishUpload(on_complete, Roe<T>{freed.error()});
                  return;
                }
                FinishUpload(on_complete, upload());
              });
            },
            Tr("blob.quota.confirm"));
      });
    }

    template <typename T>
    static void BeginUploadAttempt(std::function<Roe<T>()> upload, std::function<void(Roe<T>)> on_complete,
                                   std::function<Roe<BlobQuotaRecoveryPlan>(const Error& quota_error)> plan_recovery,
                                   std::function<Roe<void>(const std::string& blob_id)> free_slot) {
      AppRuntime::PostWorkerNormal([upload = std::move(upload), on_complete = std::move(on_complete),
                                    plan_recovery = std::move(plan_recovery),
                                    free_slot = std::move(free_slot)]() mutable {
        auto result = upload();
        if (result) {
          FinishUpload(on_complete, std::move(result));
          return;
        }
        if (!IsBlobQuotaError(result.error())) {
          FinishUpload(on_complete, Roe<T>{result.error()});
          return;
        }

        AppRuntime::PostWorkerNormal([upload = std::move(upload), on_complete = std::move(on_complete),
                                      plan_recovery = std::move(plan_recovery), free_slot = std::move(free_slot),
                                      quota_error = result.error()]() mutable {
          PromptQuotaRecovery(std::move(upload), std::move(on_complete), std::move(free_slot),
                              plan_recovery(quota_error));
        });
      });
    }

    template <typename T>
    static void RunUploadImpl(std::function<Roe<T>()> upload, std::function<void(Roe<T>)> on_complete,
                              std::function<Roe<BlobQuotaRecoveryPlan>(const Error& quota_error)> plan_recovery,
                              std::function<Roe<void>(const std::string& blob_id)> free_slot) {
      BeginUploadAttempt(std::move(upload), std::move(on_complete), std::move(plan_recovery), std::move(free_slot));
    }

    template <typename T>
    static void RunUploadAsyncImpl(std::function<void(std::function<void(Roe<T>)>)> upload,
                                   std::function<void(Roe<T>)> on_complete,
                                   std::function<Roe<BlobQuotaRecoveryPlan>(const Error& quota_error)> plan_recovery,
                                   std::function<Roe<void>(const std::string& blob_id)> free_slot) {
      BeginUploadAttemptAsync(std::move(upload), std::move(on_complete), std::move(plan_recovery),
                              std::move(free_slot));
    }
  };
};

} // namespace pbr
