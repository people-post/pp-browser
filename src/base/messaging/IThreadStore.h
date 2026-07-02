#pragma once

#include "common/Error.h"
#include "base/ai/conversation/ConversationTypes.h"
#include "base/messaging/ThreadTypes.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

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
  virtual Roe<uint64_t> AllocateSenderSeq(const std::string& thread_id) = 0;
  virtual Roe<void> ReconcileOutbox() = 0;

  /** D017 — durable outbox index; empty until v2a-p2p populates rows. */
  virtual Roe<std::vector<std::pair<std::string, std::string>>> ListPendingOutbox() const = 0;

  virtual void Flush() = 0;
};

} // namespace pbr
