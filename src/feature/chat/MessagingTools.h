#pragma once

#include "feature/ai/IToolProvider.h"

namespace pbr {

class MessagingFacade;

// Native messaging / people / identity tools as an MCP-shaped provider.
class MessagingToolProvider : public IToolProvider {
public:
  explicit MessagingToolProvider(MessagingFacade& messaging);

  std::string Id() const override;
  std::vector<ToolDescriptor> ListTools() override;

private:
  MessagingFacade& messaging_;
};

// Convenience: register MessagingToolProvider into an existing registry.
void RegisterMessagingTools(ToolRegistry& registry, MessagingFacade& messaging);

} // namespace pbr
