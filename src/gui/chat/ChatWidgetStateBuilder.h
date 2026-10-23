#pragma once

#include "lib/ai/StructuredTextParser.h"
#include "lib/ui/ChatWidgetConfigBuilders.h"
#include "lib/ui/ChatWidgetTypes.h"
#include "common/PbrCompat.h"

#include <ui/data/DataModelHandle.h>

#include <vector>

namespace pbr {

void ApplyWidgetInits(const std::vector<WidgetInit>& inits, TurnWidgetState& state);

void RegisterChatWidgetDataTypes(ui::DataModelConstructor& ctor);

} // namespace pbr
