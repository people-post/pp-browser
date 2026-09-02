#pragma once

#include "common/Error.h"
#include "common/ContextBudget.h"
#include "common/SyncStateTypes.h"
#include "common/ThreadMemoryTypes.h"
#include "common/ThreadTypes.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

struct ClearMessagesOptions {
  bool forget_memory = false;
};

class IThreadStore {
public:
  virtual ~IThreadStore() = default;

  virtual Roe<std::vector<Thread>> ListThreads() const = 0;
  virtual Roe<std::optional<Thread>> GetThread(const std::string& thread_id) const = 0;
  virtual Roe<Thread> UpsertThread(const Thread& thread) = 0;

  /** Tests and export only — feature code uses GetMessagesPage / GetMessagesForContext (D057). */
  virtual Roe<std::vector<ThreadMessage>> GetMessages(const std::string& thread_id) const = 0;
  virtual Roe<std::vector<ThreadMessage>> GetMessagesPage(const std::string& thread_id,
                                                        std::optional<int64_t> before_display_order,
                                                        size_t limit = 100) const = 0;
  virtual Roe<std::vector<ThreadMessage>> GetMessagesForContext(const std::string& thread_id,
                                                                const ContextBudget& budget) const = 0;

  /** Durable agent memory (v3) — thread.db memory key `summary` (D070). */
  virtual Roe<std::optional<ConversationSummary>> GetThreadMemory(const std::string& thread_id) const = 0;
  virtual Roe<void> SetThreadMemory(const std::string& thread_id, const ConversationSummary& summary) = 0;
  virtual Roe<void> ClearThreadMemory(const std::string& thread_id) = 0;
  /** Text/system rows with display_order strictly greater than cursor (D040 eligibility). */
  virtual Roe<int64_t> CountContextEligibleMessagesAfter(const std::string& thread_id,
                                                         int64_t after_display_order) const = 0;
  /** D042 — count annotation rows targeting `target_message_id` (all annotation_type values). */
  virtual Roe<int64_t> CountAnnotationsForTarget(const std::string& thread_id,
                                                 const std::string& target_message_id) const = 0;
  /** Chronological text/system rows after cursor for compaction input. */
  virtual Roe<std::vector<ThreadMessage>> GetContextEligibleMessagesAfter(const std::string& thread_id,
                                                                          int64_t after_display_order) const = 0;

  virtual Roe<ThreadMessage> AppendMessage(const ThreadMessage& message) = 0;
  virtual Roe<bool> UpdateMessage(const ThreadMessage& message) = 0;
  virtual Roe<bool> HasMessageId(const std::string& thread_id, const std::string& message_id) const = 0;
  virtual Roe<void> ClearMessages(const std::string& thread_id, const ClearMessagesOptions& options) = 0;
  virtual Roe<bool> DeleteThread(const std::string& thread_id) = 0;

  /** Lookup direct thread by chat target (D062). */
  virtual Roe<std::optional<Thread>> FindDirectThread(const DirectChatTarget& target) const = 0;
  /** Outbound-only create when no mapping exists (D062). */
  virtual Roe<Thread> FindOrCreateDirectThread(const DirectChatTarget& target,
                                               const std::string& participant_contact_id,
                                               const std::string& title) = 0;

  /** Group thread lookup by wire group_id (D076). */
  virtual Roe<std::optional<Thread>> FindGroupThread(const std::string& group_id) const = 0;
  /** Create group thread shell when accepting invite / creating group. */
  virtual Roe<Thread> FindOrCreateGroupThread(const std::string& group_id, const std::string& title,
                                              const std::vector<std::string>& participant_contact_ids) = 0;
  /** v1.1 hook — export transcript up to message id for fork copy. */
  virtual Roe<std::vector<ThreadMessage>> ExportMessagesUpTo(const std::string& thread_id,
                                                             const std::optional<std::string>& max_message_id) const = 0;

  virtual Roe<uint64_t> AllocateSenderSeq(const std::string& thread_id) = 0;
  virtual Roe<uint32_t> GetChatTargetSessionEpoch(const std::string& thread_id) const = 0;
  virtual Roe<std::vector<ThreadMessage>> GetMessagesBySeqRange(const std::string& thread_id,
                                                                const SeqRangeQuery& query) const = 0;
  virtual Roe<PeerSyncState> GetPeerSyncState(const std::string& thread_id, uint32_t session_epoch) const = 0;
  virtual Roe<void> SetPeerSyncState(const std::string& thread_id, uint32_t session_epoch,
                                     const PeerSyncState& state) = 0;
  /** D068 — remove relay_visible pending/failed rows for old epoch and purge matching outbox rows. */
  virtual Roe<void> CancelOldEpochPending(const std::string& thread_id, uint32_t old_session_epoch) = 0;
  /** D014/D085 — set chat_targets.session_epoch and reset next_outgoing_seq=1. */
  virtual Roe<void> AdoptChatTargetEpoch(const std::string& thread_id, uint32_t new_session_epoch) = 0;
  /** D085 — passive adopt: cancel old pending, adopt epoch, append inbound row, persist sync_state. */
  virtual Roe<ThreadMessage> AppendMessageWithPassiveEpochAdopt(const ThreadMessage& message,
                                                                uint32_t old_session_epoch,
                                                                uint32_t new_session_epoch,
                                                                const PeerSyncState& new_sync_state) = 0;
  /** D014/D068 — local epoch bump: cancel old pending, adopt epoch+1, init fresh sync_state. */
  virtual Roe<uint32_t> BumpLocalChatTargetEpoch(const std::string& thread_id) = 0;
  virtual Roe<void> ReconcileOutbox() = 0;

  /** D017 — durable outbox index; empty until v2a-p2p populates rows. */
  virtual Roe<std::vector<std::pair<std::string, std::string>>> ListPendingOutbox() const = 0;

  virtual void Flush() = 0;
};

} // namespace pbr
