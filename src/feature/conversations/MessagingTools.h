#pragma once

#include "domain/ai/ToolRegistry.h"

namespace pbr {

class ConversationsFacade;

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
