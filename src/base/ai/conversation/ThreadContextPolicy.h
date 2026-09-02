#pragma once

#include "base/ai/conversation/ConversationTypes.h"
#include "base/ai/LlmClient.h"
#include "common/thread/ThreadMemoryTypes.h"
#include "common/thread/ThreadRecordTypes.h"

#include <string>
#include <vector>

namespace pbr {

class ThreadContextPolicy {
public:
  explicit ThreadContextPolicy(ContextBudget budget = DefaultContextBudget());

  ContextBuildResult Build(const std::vector<ThreadMessage>& messages, const std::string& system_prompt,
                           const std::string& current_user_text,
                           const std::optional<std::string>& current_user_payload = std::nullopt,
                           const std::optional<ConversationSummary>& summary = std::nullopt) const;

  std::vector<ChatMessage> BuildAssistContext(const std::vector<ThreadMessage>& messages, const std::string& prompt,
                                              const std::optional<ConversationSummary>& summary = std::nullopt) const;

private:
  ContextBudget budget_;
};

} // namespace pbr
