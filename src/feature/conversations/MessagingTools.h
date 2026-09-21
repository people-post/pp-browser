#pragma once

#include "domain/ai/ToolRegistry.h"
#include "feature/conversations/ConversationsFacade.h"

namespace pbr {

// Native messaging / people / identity tools as an MCP-shaped provider.
class MessagingToolProvider : public IToolProvider {
public:
  explicit MessagingToolProvider(ConversationsFacade& messaging);

  std::string Id() const override;
  std::vector<ToolDescriptor> ListTools() override;

private:
  ConversationsFacade& messaging_;
};

// Convenience: register MessagingToolProvider into an existing registry.
void RegisterMessagingTools(ToolRegistry& registry, ConversationsFacade& messaging);

} // namespace pbr
