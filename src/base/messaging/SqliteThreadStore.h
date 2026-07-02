#pragma once

#include "common/Module.h"
#include "base/messaging/IThreadStore.h"

#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace pbr {

class SqliteThreadStore : public Module, public IThreadStore {
public:
  explicit SqliteThreadStore(std::string data_dir);
  ~SqliteThreadStore() override;

  Roe<std::vector<Thread>> ListThreads() const override;
  Roe<std::optional<Thread>> GetThread(const std::string& thread_id) const override;
  Roe<Thread> UpsertThread(const Thread& thread) override;
  Roe<std::vector<ThreadMessage>> GetMessages(const std::string& thread_id) const override;
  Roe<std::vector<ThreadMessage>> GetMessagesPage(const std::string& thread_id,
                                                  std::optional<int64_t> before_display_order,
                                                  size_t limit) const override;
  Roe<std::vector<ThreadMessage>> GetMessagesForContext(const std::string& thread_id,
                                                        const ContextBudget& budget) const override;
  Roe<ThreadMessage> AppendMessage(const ThreadMessage& message) override;
  Roe<bool> UpdateMessage(const ThreadMessage& message) override;
  Roe<bool> HasMessageId(const std::string& thread_id, const std::string& message_id) const override;
  Roe<void> ClearMessages(const std::string& thread_id, const ClearMessagesOptions& options) override;
  Roe<bool> DeleteThread(const std::string& thread_id) override;
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
  Roe<ThreadMessage> ReadMessageRow(sqlite3_stmt* stmt) const;
  Roe<int64_t> NextDisplayOrder(sqlite3* thread_db) const;
  Roe<void> UpdateThreadCatalogFromMessage(const ThreadMessage& message, bool increment_unread) const;
  Roe<void> EnsureThreadDirectory(const std::string& thread_id) const;
  Roe<std::vector<ThreadMessage>> QueryMessages(const std::string& thread_id, const char* sql,
                                                std::optional<int64_t> before_display_order, size_t limit) const;

  std::string data_dir_;
  mutable std::mutex profile_mutex_;
  mutable sqlite3* profile_db_ = nullptr;
  mutable std::mutex thread_cache_mutex_;
  mutable std::unordered_map<std::string, ThreadDbHandle> thread_dbs_;
  mutable std::list<std::string> thread_lru_;
  mutable bool initialized_ = false;
};

} // namespace pbr
