#pragma once

#include "domain/ai/conversation/Conversation.h"
#include "domain/ai/conversation/ConversationTypes.h"
#include "domain/ai/conversation/IContextPolicy.h"

#include <memory>
#include <string>

namespace pbr {

class TurnCoordinator {
public:
  explicit TurnCoordinator(std::unique_ptr<IContextPolicy> policy = nullptr);

  TurnSnapshot BeginTurn(Conversation& conversation, const std::string& system_prompt,
                         const TranscriptEntry& current_turn, const ContextBudget& budget) const;

  bool CompleteTurn(Conversation& conversation, const std::string& entry_id, const std::string& assistant_raw) const;

private:
  std::unique_ptr<IContextPolicy> policy_;
};

} // namespace pbr
