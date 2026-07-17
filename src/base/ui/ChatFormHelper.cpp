#include "base/ui/ChatFormHelper.h"

#include <nlohmann/json.hpp>

#include <optional>

namespace pbr {

std::string ApplySubmitTemplate(const std::string& template_str,
                                const std::map<std::string, std::string>& values) {
  std::string out = template_str;
  for (const auto& [field_id, value] : values) {
    const std::string token = "{{" + field_id + "}}";
    size_t pos = 0;
    while ((pos = out.find(token, pos)) != std::string::npos) {
      out.replace(pos, token.size(), value);
      pos += value.size();
    }
  }
  return out;
}

std::string BuildFormSubmissionPayload(const std::string& form_id,
                                       const std::map<std::string, std::string>& values) {
  nlohmann::json payload;
  payload["type"] = "form_submission";
  payload["form_id"] = form_id;
  payload["values"] = values;
  return payload.dump();
}

std::optional<std::string> ExtractFormId(const std::string& rml) {
  constexpr const char* marker = "data-form-id=\"";
  const size_t pos = rml.find(marker);
  if (pos == std::string::npos) {
    return std::nullopt;
  }
  const size_t start = pos + std::char_traits<char>::length(marker);
  const size_t end = rml.find('"', start);
  if (end == std::string::npos || end <= start) {
    return std::nullopt;
  }
  return rml.substr(start, end - start);
}

std::string InjectEntryPlaceholders(const std::string& rml, const std::string& entry_id) {
  std::string out = rml;
  constexpr const char* placeholder = "__ENTRY__";
  size_t pos = 0;
  while ((pos = out.find(placeholder, pos)) != std::string::npos) {
    out.replace(pos, std::char_traits<char>::length(placeholder), entry_id);
    pos += entry_id.size();
  }
  return out;
}

std::map<std::string, std::string> FormValuesMap(const FormWidgetState& form) {
  std::map<std::string, std::string> values;
  for (const FormFieldRow& field : form.fields) {
    if (std::string(field.field_type.c_str()) == "checkbox") {
      values[std::string(field.id.c_str())] = field.checked ? "true" : "false";
    } else {
      values[std::string(field.id.c_str())] = std::string(field.value.c_str());
    }
  }
  return values;
}

} // namespace pbr
