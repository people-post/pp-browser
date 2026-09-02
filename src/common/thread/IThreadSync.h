#pragma once

#include "common/Error.h"
#include "common/thread/SyncStateTypes.h"
#include "common/thread/ThreadRecordTypes.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

class IThreadSync {
public:
  virtual ~IThreadSync() = default;

  virtual Roe<uint64_t> AllocateSenderSeq(const std::string& thread_id) = 0;
  virtual Roe<uint32_t> GetChatTargetSessionEpoch(const std::string& thread_id) const = 0;
  virtual Roe<std::vector<ThreadMessage>> GetMessagesBySeqRange(const std::string& thread_id,
                                                                const SeqRangeQuery& query) const = 0;
  virtual Roe<PeerSyncState> GetPeerSyncState(const std::string& thread_id, uint32_t session_epoch) const = 0;
  virtual Roe<void> SetPeerSyncState(const std::string& thread_id, uint32_t session_epoch,
                                     const PeerSyncState& state) = 0;
  virtual Roe<void> CancelOldEpochPending(const std::string& thread_id, uint32_t old_session_epoch) = 0;
  virtual Roe<void> AdoptChatTargetEpoch(const std::string& thread_id, uint32_t new_session_epoch) = 0;
  virtual Roe<ThreadMessage> AppendMessageWithPassiveEpochAdopt(const ThreadMessage& message,
                                                                uint32_t old_session_epoch,
                                                                uint32_t new_session_epoch,
                                                                const PeerSyncState& new_sync_state) = 0;
  virtual Roe<uint32_t> BumpLocalChatTargetEpoch(const std::string& thread_id) = 0;
  virtual Roe<void> ReconcileOutbox() = 0;
  virtual Roe<std::vector<std::pair<std::string, std::string>>> ListPendingOutbox() const = 0;
};

} // namespace pbr
