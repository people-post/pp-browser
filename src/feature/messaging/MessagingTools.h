#pragma once

#include "feature/ai/ToolRegistry.h"

namespace pbr {

class MessagingHub;

void RegisterMessagingTools(ToolRegistry& registry, MessagingHub& hub);

} // namespace pbr
