#pragma once

#include "base/crypto/CryptoTypes.h"
#include "base/crypto/IDekConsumer.h"
#include "common/Module.h"
#include "base/messaging/IThreadStore.h"

#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/PbrCompat.h"

struct sqlite3;
struct sqlite3_stmt;

namespace pbr {

class SqliteThreadStore : public Module, public IThreadStore, public IDekConsumer {
public:
  explicit SqliteThreadStore(std::string data_dir);
  ~SqliteThreadStore() override;

  Roe<void> SetDek(ByteVector dek) override;
  void ClearDek() override;

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

  std::string ProfileDbPath() const;

private:
  struct ThreadDbHandle {
    sqlite3* db = nullptr;
  };

  Roe<void> EnsureInitialized() const;
  Roe<void> OpenProfileDb() const;
  Roe<sqlite3*> OpenThreadDb(const std::string& thread_id) const;
  void CloseThreadDb(const std::string& thread_id) const;
  void TouchThreadLru(const std::string& thread_id) const;
  void EvictThreadDbsIfNeeded() const;

  Roe<void> WipeLegacyJsonIfPresent() const;
  Roe<void> RepairOrphanThreadDirs() const;
  Roe<void> RequireDek() const;
  Roe<ThreadMessage> ReadMessageRow(const std::string& thread_id, sqlite3_stmt* stmt) const;
  Roe<ByteVector> EncryptMessageContent(const std::string& thread_id, const ThreadMessage& message) const;
  Roe<std::optional<ByteVector>> EncryptPreviewBlob(const std::string& thread_id,
                                                   const std::string& preview) const;
  Roe<std::string> DecryptPreviewBlob(const std::string& thread_id, const void* blob, int blob_size) const;
  Roe<int64_t> NextDisplayOrder(sqlite3* thread_db) const;
  Roe<void> UpdateThreadCatalogFromMessage(const ThreadMessage& message, bool increment_unread) const;
  Roe<void> BindMessageInsert(sqlite3_stmt* stmt, const std::string& thread_id, const ThreadMessage& message,
                              const ByteVector& content_enc) const;
  Roe<void> BindMessageUpdate(sqlite3_stmt* stmt, const std::string& thread_id, const ThreadMessage& message,
                              const ByteVector& content_enc) const;
  Roe<void> EnsureThreadDirectory(const std::string& thread_id) const;
  Roe<std::vector<ThreadMessage>> QueryMessages(const std::string& thread_id, const char* sql,
                                                std::optional<int64_t> before_display_order, size_t limit) const;
  Roe<void> UpsertChatTarget(const DirectChatTarget& target, const std::string& participant_contact_id,
                             const std::string& local_thread_id) const;
  Roe<void> ClearChatTargetThreadLink(const std::string& thread_id) const;
  void ClearChatTargetThreadLinkUnlocked(const std::string& thread_id) const;
  Roe<void> UpsertOutboxRow(const std::string& message_id, const std::string& thread_id) const;
  Roe<void> RemoveOutboxRow(const std::string& message_id) const;
  Roe<Thread> ReadThreadRow(sqlite3_stmt* stmt) const;
  Roe<DirectChatTarget> DirectTargetForThread(const Thread& thread) const;
  Roe<void> EnsurePeerSyncState(const std::string& thread_id, const DirectChatTarget& target,
                                uint32_t session_epoch) const;
  Roe<void> CancelOldEpochPendingUnlocked(sqlite3* thread_db, const std::string& thread_id,
                                          uint32_t old_session_epoch) const;
  Roe<void> AdoptChatTargetEpochUnlocked(const std::string& thread_id, uint32_t new_session_epoch) const;
  Roe<void> UpsertPeerSyncStateUnlocked(sqlite3* thread_db, const DirectChatTarget& target, uint32_t session_epoch,
                                        const PeerSyncState& state) const;

  std::string data_dir_;
  std::string profile_id_;
  ByteVector dek_;
  mutable std::mutex dek_mutex_;
  mutable std::mutex profile_mutex_;
  mutable sqlite3* profile_db_ = nullptr;
  mutable std::mutex thread_cache_mutex_;
  mutable std::unordered_map<std::string, ThreadDbHandle> thread_dbs_;
  mutable std::list<std::string> thread_lru_;
  mutable bool initialized_ = false;
};

} // namespace pbr
