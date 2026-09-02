#include "base/ai/conversation/ThreadCompactionService.h"

#include "base/ai/LlmClient.h"
#include "common/chat/MessagingLimits.h"
#include "common/thread/ThreadMemoryTypes.h"
#include "common/thread/ThreadRecordTypes.h"
#include "foundation/runtime/AppRuntime.h"
#include "common/Utilities.h"

#include <sstream>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

std::string BuildCompactionPrompt(const std::vector<ThreadMessage>& messages,
                                  const std::optional<ConversationSummary>& existing) {
  std::ostringstream transcript;
  for (const ThreadMessage& message : messages) {
    transcript << message.sender_contact_id << ": " << message.text << "\n";
  }

  std::ostringstream prompt;
  prompt << "Summarize the following conversation transcript for long-term agent memory.\n"
         << "Preserve key facts, preferences, decisions, and open questions.\n"
         << "Write concise prose (no bullet lists unless necessary). Max "
         << kMaxSummaryBytes << " bytes.\n\n";
  if (existing && !existing->text.empty()) {
    prompt << "Previous summary (merge and update; do not repeat verbatim):\n" << existing->text << "\n\n";
  }
  prompt << "New transcript segment:\n" << transcript.str();
  return prompt.str();
}

} // namespace

ThreadCompactionService::ThreadCompactionService(IThreadCatalog& catalog, IThreadTranscript& transcript,
                                                 IThreadMemory& memory, LlmClient* llm)
    : catalog_(catalog), transcript_(transcript), memory_(memory), llm_(llm) {
  redirectLogger("ThreadCompactionService");
}

void ThreadCompactionService::MaybeCompactAsync(const std::string& thread_id) {
  if (!llm_) {
    return;
  }

  {
    std::lock_guard lock(pending_mutex_);
    if (pending_threads_.contains(thread_id)) {
      return;
    }
    pending_threads_.insert(thread_id);
  }

  AppRuntime::PostWorkerNormal([this, thread_id]() {
    RunCompaction(thread_id);
    {
      std::lock_guard lock(pending_mutex_);
      pending_threads_.erase(thread_id);
    }
  });
}

void ThreadCompactionService::RunCompaction(const std::string& thread_id) {
  auto thread = catalog_.GetThread(thread_id);
  if (!thread || !*thread || (*thread)->kind != ThreadKind::Ai) {
    return;
  }

  const int64_t cursor = [&]() -> int64_t {
    auto memory = memory_.GetThreadMemory(thread_id);
    if (!memory || !memory->has_value()) {
      return 0;
    }
    return memory->value().compacted_through_display_order.value_or(0);
  }();

  auto eligible_count = transcript_.CountContextEligibleMessagesAfter(thread_id, cursor);
  if (!eligible_count || *eligible_count <= kCompactionTurnThreshold) {
    return;
  }

  auto messages = transcript_.GetContextEligibleMessagesAfter(thread_id, cursor);
  if (!messages || messages->size() <= static_cast<size_t>(kCompactionMinTurnsKept * 2)) {
    return;
  }

  const size_t keep_tail = static_cast<size_t>(kCompactionMinTurnsKept * 2);
  const size_t compact_count = messages->size() - keep_tail;
  if (compact_count == 0) {
    return;
  }

  std::vector<ThreadMessage> to_compact(messages->begin(), messages->begin() + static_cast<std::ptrdiff_t>(compact_count));
  const int64_t compacted_through = to_compact.back().display_order;

  std::optional<ConversationSummary> existing;
  if (auto memory = memory_.GetThreadMemory(thread_id)) {
    existing = *memory;
  }

  const std::string user_prompt = BuildCompactionPrompt(to_compact, existing);
  const auto summary_text =
      llm_->Complete("You produce durable conversation summaries for an AI assistant.", user_prompt);
  if (!summary_text) {
    log().warning << "Compaction failed for thread " << thread_id << ": " << summary_text.error().message;
    return;
  }

  if (summary_text.value().size() > kMaxSummaryBytes) {
    log().warning << "Compaction summary too large for thread " << thread_id;
    return;
  }

  ConversationSummary summary;
  summary.schema_version = 1;
  summary.text = summary_text.value();
  summary.version = existing && existing->version > 0 ? existing->version + 1 : 1;
  summary.compacted_through_display_order = compacted_through;
  summary.updated_at = util::NowUnixMs();

  if (auto saved = memory_.SetThreadMemory(thread_id, summary); !saved) {
    log().warning << "Failed to persist summary for thread " << thread_id << ": " << saved.error().message;
  }
}

} // namespace pbr
