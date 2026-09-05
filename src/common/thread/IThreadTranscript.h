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
  virtual Roe<bool> HasMessageId(const std::string& thread_id, const std::string& message_id) const = 0;
  virtual Roe<void> ClearMessages(const std::string& thread_id, const ClearMessagesOptions& options) = 0;
  virtual Roe<std::vector<ThreadMessage>> ExportMessagesUpTo(const std::string& thread_id,
                                                             const std::optional<std::string>& max_message_id) const = 0;
};

} // namespace pbr
