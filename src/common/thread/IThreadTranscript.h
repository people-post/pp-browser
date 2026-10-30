#pragma once

#include "common/Error.h"
#include "common/thread/ContextBudget.h"
#include "common/thread/ThreadRecordTypes.h"

#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

struct ClearMessagesOptions {
  bool forget_memory = false;
};

class IThreadTranscript {
public:
  virtual ~IThreadTranscript() = default;

  /** Tests and export only — feature code uses GetMessagesPage / GetMessagesForContext (D057). */
  virtual Roe<std::vector<ThreadMessage>> GetMessages(const std::string& thread_id) const = 0;
  virtual Roe<std::vector<ThreadMessage>> GetMessagesPage(const std::string& thread_id,
                                                          std::optional<int64_t> before_display_order,
                                                          size_t limit = 100) const = 0;
  virtual Roe<std::vector<ThreadMessage>> GetMessagesForContext(const std::string& thread_id,
                                                                const ContextBudget& budget) const = 0;
  virtual Roe<int64_t> CountContextEligibleMessagesAfter(const std::string& thread_id,
                                                         int64_t after_display_order) const = 0;
  virtual Roe<int64_t> CountAnnotationsForTarget(const std::string& thread_id,
                                                 const std::string& target_message_id) const = 0;
  virtual Roe<std::vector<ThreadMessage>> GetContextEligibleMessagesAfter(const std::string& thread_id,
                                                                          int64_t after_display_order) const = 0;
  virtual Roe<ThreadMessage> AppendMessage(const ThreadMessage& message) = 0;
  virtual Roe<bool> UpdateMessage(const ThreadMessage& message) = 0;
  /**
   * Insert, or overwrite the row with the same id (keeps its place in the thread) — for a record one
   * device rewrites as it goes (a group call's summary). The SQLite store does it in one statement;
   * the default (test doubles) is update-then-append.
   */
  virtual Roe<ThreadMessage> UpsertMessage(const ThreadMessage& message) {
    auto updated = UpdateMessage(message);
    if (updated && *updated) {
      return message;
    }
    return AppendMessage(message);
  }
  /** One message by id (nullopt when unknown). Default: scan the newest page (test doubles). */
  virtual Roe<std::optional<ThreadMessage>> GetMessage(const std::string& thread_id, const std::string& message_id) const {
    auto page = GetMessagesPage(thread_id, std::nullopt, 10000);
    if (!page) {
      return page.error();
    }
    for (const ThreadMessage& message : *page) {
      if (message.id == message_id) {
        return std::optional<ThreadMessage>(message);
      }
    }
    return std::optional<ThreadMessage>();
  }
  /** Annotation rows aimed at `target_message_id`. Default: scan the newest page (test doubles). */
  virtual Roe<std::vector<ThreadMessage>> GetAnnotationsForTarget(const std::string& thread_id,
                                                                  const std::string& target_message_id) const {
    auto page = GetMessagesPage(thread_id, std::nullopt, 10000);
    if (!page) {
      return page.error();
    }
    std::vector<ThreadMessage> out;
    for (const ThreadMessage& message : *page) {
      if (message.content_type == ChatContentType::Annotation && message.target_message_id == target_message_id) {
        out.push_back(message);
      }
    }
    return out;
  }
  /**
   * "Delete for me": wipe the message's content (text, payload, attachment / image references) and keep an
   * id-only tombstone (`ChatContentType::Deleted`), so dedupe and history sync still see the id and never
   * bring the message back. false when the id is unknown. Default: unsupported (test doubles).
   */
  virtual Roe<bool> DeleteMessageContent(const std::string& thread_id, const std::string& message_id) {
    (void)thread_id;
    (void)message_id;
    return Error("DeleteMessageContent not supported");
  }
  virtual Roe<bool> HasMessageId(const std::string& thread_id, const std::string& message_id) const = 0;
  virtual Roe<void> ClearMessages(const std::string& thread_id, const ClearMessagesOptions& options) = 0;
  virtual Roe<std::vector<ThreadMessage>> ExportMessagesUpTo(const std::string& thread_id,
                                                             const std::optional<std::string>& max_message_id) const = 0;
};

} // namespace pbr
