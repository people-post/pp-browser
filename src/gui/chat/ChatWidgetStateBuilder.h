#pragma once

#include "domain/ai/StructuredTextParser.h"
#include "domain/ui/ChatWidgetConfigBuilders.h"
#include "domain/ui/ChatWidgetTypes.h"
#include "common/PbrCompat.h"

#include <ui/data/DataModelHandle.h>

#include <vector>

namespace pbr {

void ApplyWidgetInits(const std::vector<WidgetInit>& inits, TurnWidgetState& state);

void RegisterChatWidgetDataTypes(ui::DataModelConstructor& ctor);

} // namespace pbr
