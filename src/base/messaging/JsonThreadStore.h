#pragma once

#include "common/Module.h"
#include "base/people/ContactTypes.h"
#include "base/messaging/IThreadStore.h"

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

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
  Roe<ThreadMessage> AppendMessage(const ThreadMessage& message) override;
  Roe<bool> UpdateMessage(const ThreadMessage& message) override;
  Roe<bool> HasMessageId(const std::string& thread_id, const std::string& message_id) const override;
  Roe<void> ClearMessages(const std::string& thread_id, const ClearMessagesOptions& options) override;
  Roe<bool> DeleteThread(const std::string& thread_id) override;
  Roe<std::optional<Thread>> FindDirectThread(const DirectChatTarget& target) const override;
  Roe<Thread> FindOrCreateDirectThread(const DirectChatTarget& target, const std::string& participant_contact_id,
                                       const std::string& title) override;
  Roe<uint64_t> AllocateSenderSeq(const std::string& thread_id) override;
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
  mutable bool dirty_ = false;
};

} // namespace pbr
