#pragma once

#include "base/ai/StructuredTextParser.h"
#include "feature/chat/ChatWidgetTypes.h"

#include <RmlUi/Core/DataModelHandle.h>

#include <map>
#include <string>

namespace pbr {

FormWidgetState BuildFormWidgetState(const nlohmann::json& config);
CalendarWidgetState BuildCalendarWidgetState(const nlohmann::json& config);

void ApplyWidgetInits(const std::vector<WidgetInit>& inits, TurnWidgetState& state);

std::map<std::string, std::string> FormValuesMap(const FormWidgetState& form);

void RegisterChatWidgetDataTypes(Rml::DataModelConstructor& ctor);

} // namespace pbr
