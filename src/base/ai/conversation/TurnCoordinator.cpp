#include "agent/conversation/TurnCoordinator.h"

#include "agent/conversation/SlidingWindowContextPolicy.h"

namespace pbr {

TurnCoordinator::TurnCoordinator(std::unique_ptr<IContextPolicy> policy)
    : policy_(policy ? std::move(policy) : std::make_unique<SlidingWindowContextPolicy>()) {}

TurnSnapshot TurnCoordinator::BeginTurn(Conversation& conversation, const std::string& system_prompt,
                                        const TranscriptEntry& current_turn, const ContextBudget& budget) const {
  ContextBuildResult built = policy_->Build(system_prompt, conversation, current_turn, budget);
  return TurnSnapshot{
      .entry_id = current_turn.id,
      .turn_index = current_turn.turn_index,
      .messages = std::move(built.messages),
      .provenance = built.provenance,
  };
}

bool TurnCoordinator::CompleteTurn(Conversation& conversation, const std::string& entry_id,
                                    const std::string& assistant_raw) const {
  return conversation.CompleteTurn(entry_id, assistant_raw);
}

} // namespace pbr
