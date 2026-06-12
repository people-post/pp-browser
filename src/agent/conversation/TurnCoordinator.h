#pragma once

#include "agent/conversation/Conversation.h"
#include "agent/conversation/ConversationTypes.h"
#include "agent/conversation/IContextPolicy.h"

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
