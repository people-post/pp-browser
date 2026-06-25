#pragma once

#include "base/ai/conversation/ConversationTypes.h"
#include "base/ai/LlmClient.h"
#include "base/messaging/ThreadTypes.h"

#include <string>
#include <vector>

namespace pbr {

class ThreadContextPolicy {
public:
  explicit ThreadContextPolicy(ContextBudget budget = DefaultContextBudget());

  ContextBuildResult Build(const std::vector<ThreadMessage>& messages, const std::string& system_prompt,
                           const std::string& current_user_text,
                           const std::optional<std::string>& current_user_payload = std::nullopt) const;

  std::vector<ChatMessage> BuildAssistContext(const std::vector<ThreadMessage>& messages,
                                              const std::string& prompt) const;

private:
  ContextBudget budget_;
};

} // namespace pbr
