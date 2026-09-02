#pragma once

#include "common/Module.h"
#include "common/thread/IThreadStore.h"

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

class JsonThreadStore : public Module, public IThreadStore {
public:
  explicit JsonThreadStore(std::string data_dir);

  Roe<std::vector<Thread>> ListThreads() const override;
  Roe<std::optional<Thread>> GetThread(const std::string& thread_id) const override;
  Roe<Thread> UpsertThread(const Thread& thread) override;
  Roe<std::vector<ThreadMessage>> GetMessages(const std::string& thread_id) const override;
  Roe<std::vector<ThreadMessage>> GetMessagesPage(const std::string& thread_id,
                                                  std::optional<int64_t> before_display_order,
                                                  size_t limit) const override;
  Roe<std::vector<ThreadMessage>> GetMessagesForContext(const std::string& thread_id,
                                                        const ContextBudget& budget) const override;
  Roe<std::optional<ConversationSummary>> GetThreadMemory(const std::string& thread_id) const override;
  Roe<void> SetThreadMemory(const std::string& thread_id, const ConversationSummary& summary) override;
  Roe<void> ClearThreadMemory(const std::string& thread_id) override;
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
  Roe<bool> DeleteThread(const std::string& thread_id) override;
  Roe<std::optional<Thread>> FindDirectThread(const DirectChatTarget& target) const override;
  Roe<Thread> FindOrCreateDirectThread(const DirectChatTarget& target, const std::string& participant_contact_id,
                                       const std::string& title) override;
  Roe<std::optional<Thread>> FindGroupThread(const std::string& group_id) const override;
  Roe<Thread> FindOrCreateGroupThread(const std::string& group_id, const std::string& title,
                                      const std::vector<std::string>& participant_contact_ids) override;
  Roe<std::vector<ThreadMessage>> ExportMessagesUpTo(const std::string& thread_id,
                                                     const std::optional<std::string>& max_message_id) const override;
  Roe<uint64_t> AllocateSenderSeq(const std::string& thread_id) override;
  Roe<uint32_t> GetChatTargetSessionEpoch(const std::string& thread_id) const override;
  Roe<std::vector<ThreadMessage>> GetMessagesBySeqRange(const std::string& thread_id,
                                                        const SeqRangeQuery& query) const override;
  Roe<PeerSyncState> GetPeerSyncState(const std::string& thread_id, uint32_t session_epoch) const override;
  Roe<void> SetPeerSyncState(const std::string& thread_id, uint32_t session_epoch,
                             const PeerSyncState& state) override;
  Roe<void> CancelOldEpochPending(const std::string& thread_id, uint32_t old_session_epoch) override;
  Roe<void> AdoptChatTargetEpoch(const std::string& thread_id, uint32_t new_session_epoch) override;
  Roe<ThreadMessage> AppendMessageWithPassiveEpochAdopt(const ThreadMessage& message, uint32_t old_session_epoch,
                                                        uint32_t new_session_epoch,
                                                        const PeerSyncState& new_sync_state) override;
  Roe<uint32_t> BumpLocalChatTargetEpoch(const std::string& thread_id) override;
  Roe<void> ReconcileOutbox() override;
  Roe<std::vector<std::pair<std::string, std::string>>> ListPendingOutbox() const override;
  void Flush() override;

private:
  Roe<void> EnsureLoaded() const;
  Roe<void> SaveIndex() const;
  Roe<void> SaveMessages(const std::string& thread_id) const;
  std::string ThreadPath(const std::string& thread_id) const;
  std::string IndexPath() const;
  int64_t NextDisplayOrder(const std::string& thread_id) const;

  std::string data_dir_;
  mutable std::mutex mutex_;
  mutable bool loaded_ = false;
  mutable std::vector<Thread> threads_;
  mutable std::unordered_map<std::string, std::vector<ThreadMessage>> messages_;
  mutable std::unordered_map<std::string, ConversationSummary> memory_;
  mutable bool dirty_ = false;
};

} // namespace pbr
