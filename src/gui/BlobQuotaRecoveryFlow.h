#pragma once

#include "domain/net/BlobQuotaUtil.h"
#include "foundation/runtime/AppRuntime.h"
#include "common/Error.h"
#include "gui/UserFeedback.h"

#include "foundation/i18n/LocalizationService.h"
#include "domain/messaging/AttachmentCache.h"

#include <functional>
#include <utility>
#include "common/PbrCompat.h"

namespace pbr {

/** UI orchestration for R009: confirm, delete oldest remote blob, retry upload. */
struct BlobQuotaRecoveryFlow {
  static void RunVoidUpload(std::function<Roe<void>()> upload, std::function<void(Roe<void>)> on_complete,
                              std::function<Roe<BlobQuotaRecoveryPlan>()> plan_recovery,
                              std::function<Roe<void>()> free_slot);

  template <typename T>
  static void RunUpload(std::function<Roe<T>()> upload, std::function<void(Roe<T>)> on_complete,
                        std::function<Roe<BlobQuotaRecoveryPlan>()> plan_recovery,
                        std::function<Roe<void>()> free_slot) {
    Detail::RunUploadImpl(std::move(upload), std::move(on_complete), std::move(plan_recovery), std::move(free_slot));
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
    static void PromptQuotaRecovery(std::function<Roe<T>()> upload, std::function<void(Roe<T>)> on_complete,
                                    std::function<Roe<void>()> free_slot, Roe<BlobQuotaRecoveryPlan> plan) {
      AppRuntime::PostUI([upload = std::move(upload), on_complete = std::move(on_complete),
                          free_slot = std::move(free_slot), plan = std::move(plan)]() mutable {
        if (!plan) {
          FinishUpload(on_complete, Roe<T>{plan.error()});
          return;
        }

        UserFeedback::Confirm(
            Tr("blob.quota.title"), BuildQuotaConfirmMessage(*plan),
            [upload = std::move(upload), on_complete = std::move(on_complete), free_slot = std::move(free_slot)](
                const bool confirmed) mutable {
              if (!confirmed) {
                FinishUpload(on_complete, Roe<T>{Error(Tr("blob.quota.cancelled"))});
                return;
              }
              AppRuntime::PostWorkerNormal([upload = std::move(upload), on_complete = std::move(on_complete),
                                            free_slot = std::move(free_slot)]() mutable {
                auto freed = free_slot();
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
                                   std::function<Roe<BlobQuotaRecoveryPlan>()> plan_recovery,
                                   std::function<Roe<void>()> free_slot) {
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
                                      plan_recovery = std::move(plan_recovery),
                                      free_slot = std::move(free_slot)]() mutable {
          PromptQuotaRecovery(std::move(upload), std::move(on_complete), std::move(free_slot), plan_recovery());
        });
      });
    }

    template <typename T>
    static void RunUploadImpl(std::function<Roe<T>()> upload, std::function<void(Roe<T>)> on_complete,
                              std::function<Roe<BlobQuotaRecoveryPlan>()> plan_recovery,
                              std::function<Roe<void>()> free_slot) {
      BeginUploadAttempt(std::move(upload), std::move(on_complete), std::move(plan_recovery), std::move(free_slot));
    }
  };
};

} // namespace pbr
