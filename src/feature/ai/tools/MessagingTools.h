#pragma once

#include "feature/ai/ToolRegistry.h"
#include "feature/messaging/MessagingHub.h"

namespace pbr {

void RegisterMessagingTools(ToolRegistry& registry, MessagingHub& hub);

} // namespace pbr
