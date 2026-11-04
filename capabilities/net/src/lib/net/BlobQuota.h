#pragma once

#include "lib/net/BlobClient.h"
#include "common/Error.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Space on the files relay (`/files`, attachments) to free: the only quota this client can run into and free.
 * `/blobs` holds profile photos alone, under their own limit.
 */
struct BlobQuotaRecoveryPlan {
  /**
   * What the dialog names: the oldest of the files to delete, with `byte_length` the bytes they free together and
   * `created_at` the oldest one's completion time.
   */
  RelayBlobRecord blob_to_delete;
  /** Every id the confirmed recovery deletes: as many as free enough. */
  std::vector<std::string> blob_ids_to_delete;
  /** The files relay's "used / total" (bytes). */
  RelayFileUsage file_usage;
};

bool IsBlobQuotaError(const Error& err);

/** Whether a quota error is the files relay's (`QUOTA_FILE_BYTES`): the one a recovery can free. */
bool IsRelayFileQuotaError(const Error& err);

/** The size a refused `/files/start` asked for (HttpBlobClient notes it in the detail); 0 when not there. */
uint64_t RelayFileQuotaNeededBytes(const Error& err);

/**
 * The files relay is full (`QUOTA_FILE_BYTES`): from `/files/list` (complete files, the newest 500) the oldest by
 * completion, as many as it takes for `needed_bytes` to fit under the quota (at least one). None of them is
 * deleted here. When even all of them would not free enough, a QuotaExceeded error and no plan: nothing is
 * deleted for an upload that still could not go.
 */
Roe<BlobQuotaRecoveryPlan> PlanOldestRelayFileDeletion(IBlobClient& blob, const std::string& relay_user_id,
                                                       uint64_t needed_bytes);

/** `/files/delete` of one id (an attachment's relay copy). */
Roe<void> DeleteRelayFile(IBlobClient& blob, const std::string& relay_user_id, const std::string& blob_id);

/** What a confirmed recovery calls for each id of the plan. */
using BlobQuotaFreeSlot = std::function<Roe<void>(const std::string& blob_id)>;

} // namespace pbr
