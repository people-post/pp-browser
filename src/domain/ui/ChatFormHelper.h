#pragma once

#include "domain/ui/ChatWidgetTypes.h"

#include <map>
#include <optional>
#include <string>

namespace pbr {

std::string ApplySubmitTemplate(const std::string& template_str,
                                const std::map<std::string, std::string>& values);
std::string BuildFormSubmissionPayload(const std::string& form_id,
                                       const std::map<std::string, std::string>& values);

std::optional<std::string> ExtractFormId(const std::string& rml);
std::string InjectEntryPlaceholders(const std::string& rml, const std::string& entry_id);
std::map<std::string, std::string> FormValuesMap(const FormWidgetState& form);

} // namespace pbr
