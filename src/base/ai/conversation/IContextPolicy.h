#pragma once

#include "base/ai/conversation/Conversation.h"
#include "base/ai/conversation/ConversationTypes.h"

#include <string>

namespace pbr {

class IContextPolicy {
public:
  virtual ~IContextPolicy() = default;

  virtual ContextBuildResult Build(const std::string& system_prompt, const Conversation& conversation,
                                   const TranscriptEntry& current_turn, const ContextBudget& budget) const = 0;
};

} // namespace pbr
