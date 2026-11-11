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
  /** The receive state of the peer's numbering at `session_epoch` — of one of its devices with `sender_device` (M021). */
  virtual Roe<PeerSyncState> GetPeerSyncState(const std::string& thread_id, uint32_t session_epoch,
                                              const std::string& sender_device = {}) const = 0;
  virtual Roe<void> SetPeerSyncState(const std::string& thread_id, uint32_t session_epoch, const PeerSyncState& state,
                                     const std::string& sender_device = {}) = 0;
  virtual Roe<void> CancelOldEpochPending(const std::string& thread_id, uint32_t old_session_epoch) = 0;
  virtual Roe<void> AdoptChatTargetEpoch(const std::string& thread_id, uint32_t new_session_epoch) = 0;
  virtual Roe<ThreadMessage> AppendMessageWithPassiveEpochAdopt(const ThreadMessage& message,
                                                                uint32_t old_session_epoch, uint32_t new_session_epoch,
                                                                const PeerSyncState& new_sync_state) = 0;
  virtual Roe<uint32_t> BumpLocalChatTargetEpoch(const std::string& thread_id) = 0;
  virtual Roe<void> ReconcileOutbox() = 0;
  virtual Roe<std::vector<std::pair<std::string, std::string>>> ListPendingOutbox() const = 0;
  /**
   * Takes away a message of ours that never went out, when the user sends it again as a new one (#210): only a row
   * this device sent (`kLocalSelfContactId`), meant for the wire, still pending or failed — with its "did not go out"
   * lines (`send-failure-<id>`, `-group`) and its outbox record. Nothing is left in its place and nothing is sent.
   * false, and nothing removed, for any other row (delivered, received, someone else's, unknown).
   */
  virtual Roe<bool> RemoveUnsentMessage(const std::string& thread_id, const std::string& message_id) = 0;
};

} // namespace pbr
