#pragma once

#include "feature/ai/ToolRegistry.h"

namespace pbr {

class MessagingFacade;

void RegisterMessagingTools(ToolRegistry& registry, MessagingFacade& messaging);

} // namespace pbr
