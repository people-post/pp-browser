#pragma once

#include "agent/ToolRegistry.h"
#include "messaging/MessagingHub.h"

namespace pbr {

void RegisterMessagingTools(ToolRegistry& registry, MessagingHub& hub);

} // namespace pbr
