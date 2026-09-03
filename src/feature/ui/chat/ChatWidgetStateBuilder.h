#pragma once

#include "domain/ai/StructuredTextParser.h"
#include "domain/ui/ChatWidgetTypes.h"
#include "common/PbrCompat.h"

#include <RmlUi/Core/DataModelHandle.h>

#include <map>
#include <string>

namespace pbr {

FormWidgetState BuildFormWidgetState(const Object& config);
CalendarWidgetState BuildCalendarWidgetState(const Object& config);

void ApplyWidgetInits(const std::vector<WidgetInit>& inits, TurnWidgetState& state);

void RegisterChatWidgetDataTypes(Rml::DataModelConstructor& ctor);

} // namespace pbr
