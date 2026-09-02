#pragma once

#include "domain/ai/conversation/IContextPolicy.h"

namespace pbr {

class SlidingWindowContextPolicy final : public IContextPolicy {
public:
  ContextBuildResult Build(const std::string& system_prompt, const Conversation& conversation,
                           const TranscriptEntry& current_turn, const ContextBudget& budget) const override;
};

} // namespace pbr
