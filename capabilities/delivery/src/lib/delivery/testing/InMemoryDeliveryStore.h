#pragma once

// Delivery's stores in memory — the threads (IThreadStore) and the peer sessions (IPeerSessionStore) over shared
// rows, and the control lane's seen ids — for tests of capabilities that keep records through delivery's ports
// (CR024) instead of the app's SQLite storage. Never linked into the app or pp-node.
//
// It follows the SQLite stores where callers see the difference: a direct thread and the account-level session
// with its peer are one row, so they share the session epoch (a session saved first gets its thread later); a
// direct thread starts at epoch 1 and sender seq 1, a group numbers per group, a device pair's session numbers on
// its own and restarts at 1 in a new epoch; messages get the next display order and a page is the newest `limit`
// in ascending order; a thread keeps `listed` once set. What it leaves out: encryption at rest, the outbox
// (ListPendingOutbox is empty), and context budgets (every message is eligible).

#include "common/thread/IThreadStore.h"
#include "lib/delivery/IControlLaneSeenStore.h"
#include "lib/delivery/IPeerSessionStore.h"

#include <map>
#include <set>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

class InMemoryDeliveryStore : public IThreadStore, public IPeerSessionStore {
public:
  // IThreadCatalog
  Roe<std::vector<Thread>> ListThreads() const override;
  Roe<std::optional<Thread>> GetThread(const std::string& thread_id) const override;
  Roe<Thread> UpsertThread(const Thread& thread) override;
  Roe<bool> DeleteThread(const std::string& thread_id) override;
  Roe<std::optional<Thread>> FindDirectThread(const DirectChatTarget& target) const override;
  Roe<Thread> FindOrCreateDirectThread(const DirectChatTarget& target, const std::string& participant_contact_id,
                                       const std::string& title) override;
  Roe<std::optional<Thread>> FindGroupThread(const std::string& group_id) const override;
  Roe<Thread> FindOrCreateGroupThread(const std::string& group_id, const std::string& title,
                                      const std::vector<std::string>& participant_contact_ids) override;

  // IThreadTranscript
  Roe<std::vector<ThreadMessage>> GetMessages(const std::string& thread_id) const override;
  Roe<std::vector<ThreadMessage>> GetMessagesPage(const std::string& thread_id,
                                                  std::optional<int64_t> before_display_order,
                                                  size_t limit = 100) const override;
  Roe<std::vector<ThreadMessage>> GetMessagesForContext(const std::string& thread_id,
                                                        const ContextBudget& budget) const override;
  Roe<int64_t> CountContextEligibleMessagesAfter(const std::string& thread_id,
                                                 int64_t after_display_order) const override;
  Roe<int64_t> CountAnnotationsForTarget(const std::string& thread_id,
                                         const std::string& target_message_id) const override;
  Roe<std::vector<ThreadMessage>> GetContextEligibleMessagesAfter(const std::string& thread_id,
                                                                  int64_t after_display_order) const override;
  Roe<ThreadMessage> AppendMessage(const ThreadMessage& message) override;
  Roe<bool> UpdateMessage(const ThreadMessage& message) override;
  Roe<bool> HasMessageId(const std::string& thread_id, const std::string& message_id) const override;
  Roe<void> ClearMessages(const std::string& thread_id, const ClearMessagesOptions& options) override;
  Roe<std::vector<ThreadMessage>> ExportMessagesUpTo(const std::string& thread_id,
                                                     const std::optional<std::string>& max_message_id) const override;

  // IThreadMemory
  Roe<std::optional<ConversationSummary>> GetThreadMemory(const std::string& thread_id) const override;
  Roe<void> SetThreadMemory(const std::string& thread_id, const ConversationSummary& summary) override;
  Roe<void> ClearThreadMemory(const std::string& thread_id) override;

  // IThreadSync
  Roe<uint64_t> AllocateSenderSeq(const std::string& thread_id) override;
  Roe<uint32_t> GetChatTargetSessionEpoch(const std::string& thread_id) const override;
  Roe<std::vector<ThreadMessage>> GetMessagesBySeqRange(const std::string& thread_id,
                                                        const SeqRangeQuery& query) const override;
  Roe<PeerSyncState> GetPeerSyncState(const std::string& thread_id, uint32_t session_epoch,
                                      const std::string& sender_device = {}) const override;
  Roe<void> SetPeerSyncState(const std::string& thread_id, uint32_t session_epoch, const PeerSyncState& state,
                             const std::string& sender_device = {}) override;
  Roe<void> CancelOldEpochPending(const std::string& thread_id, uint32_t old_session_epoch) override;
  Roe<void> AdoptChatTargetEpoch(const std::string& thread_id, uint32_t new_session_epoch) override;
  Roe<ThreadMessage> AppendMessageWithPassiveEpochAdopt(const ThreadMessage& message, uint32_t old_session_epoch,
                                                        uint32_t new_session_epoch,
                                                        const PeerSyncState& new_sync_state) override;
  Roe<uint32_t> BumpLocalChatTargetEpoch(const std::string& thread_id) override;
  Roe<void> ReconcileOutbox() override;
  Roe<std::vector<std::pair<std::string, std::string>>> ListPendingOutbox() const override;

  void Flush() override {}

  // IPeerSessionStore
  Roe<std::optional<PeerSessionRecord>> Load(const PeerSessionKey& key) const override;
  Roe<std::vector<PeerSessionRecord>> List() const override;
  Roe<void> Save(const PeerSessionRecord& record) override;
  Roe<uint64_t> AllocateSenderSeq(const PeerSessionKey& key) override;

private:
  /** A thread's numbering: a direct thread's chat target, or a group's. */
  struct Numbering {
    uint32_t session_epoch = 1;
    uint64_t next_outgoing_seq = 1;
  };
  /** A direct thread's target and the account-level session with that peer: one row, as in SQLite. */
  struct ChatTargetRow {
    DirectChatTarget target;
    std::string participant_contact_id;
    std::string thread_id;  // empty while only the session exists
    Numbering numbering;
    std::optional<PeerSessionRecord> session;  // its epoch lives in `numbering`
  };
  struct DeviceSessionRow {
    PeerSessionRecord session;
    uint64_t next_outgoing_seq = 1;
  };

  ChatTargetRow* TargetLocked(const DirectChatTarget& target);
  ChatTargetRow* TargetForThreadLocked(const std::string& thread_id);
  const ChatTargetRow* TargetForThreadLocked(const std::string& thread_id) const;
  Numbering* NumberingLocked(const std::string& thread_id);
  const Numbering* NumberingLocked(const std::string& thread_id) const;
  std::optional<Thread> FindDirectThreadLocked(const DirectChatTarget& target) const;
  Thread UpsertThreadLocked(const Thread& thread);
  Roe<ThreadMessage> AppendMessageLocked(const ThreadMessage& message);

  mutable std::mutex mutex_;
  std::map<std::string, Thread> threads_;
  std::vector<ChatTargetRow> targets_;
  std::map<std::tuple<std::string, std::string, CryptoChannel, std::string>, DeviceSessionRow> devices_;
  std::map<std::string, Numbering> groups_;
  /** Per thread, ascending display order. */
  std::map<std::string, std::vector<ThreadMessage>> messages_;
  std::map<std::string, ConversationSummary> memory_;
  std::map<std::tuple<std::string, uint32_t, std::string>, PeerSyncState> sync_;
};

/** IControlLaneSeenStore in memory (no pruning: a test's process is short). */
class InMemoryControlLaneSeenStore : public IControlLaneSeenStore {
public:
  Roe<bool> FirstSight(const std::string& sender, const std::string& control_id, int64_t now_ms) override;
  void Forget(const std::string& sender, const std::string& control_id) override;

private:
  std::mutex mutex_;
  std::set<std::pair<std::string, std::string>> seen_;
};

} // namespace pbr
