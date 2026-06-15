#pragma once

#include <map>
#include <optional>
#include <string>

namespace Rml {
class Context;
}

namespace pbr {

std::optional<std::string> ReadFormSubmitTemplate(Rml::Context* context, const std::string& form_element_id);
std::map<std::string, std::string> ReadFormValues(Rml::Context* context, const std::string& form_element_id);
void RestoreFormValues(Rml::Context* context, const std::string& form_element_id,
                       const std::map<std::string, std::string>& values);

std::string ApplySubmitTemplate(const std::string& template_str,
                                const std::map<std::string, std::string>& values);
std::string BuildFormSubmissionPayload(const std::string& form_id,
                                       const std::map<std::string, std::string>& values);

std::optional<std::string> ExtractFormId(const std::string& rml);
std::string InjectEntryPlaceholders(const std::string& rml, const std::string& entry_id);
std::string ExpireFormRml(std::string rml);

} // namespace pbr
