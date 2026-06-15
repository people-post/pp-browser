#include "demo/ChatFormHelper.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>

#include <nlohmann/json.hpp>

#include <sstream>

namespace pbr {

namespace {

void CollectFormFields(Rml::Element* element, std::map<std::string, std::string>& out) {
  if (!element) {
    return;
  }

  const Rml::String& tag = element->GetTagName();
  if (tag == "input" || tag == "textarea" || tag == "select") {
    if (auto* control = rmlui_dynamic_cast<Rml::ElementFormControl*>(element)) {
      const Rml::String name = element->GetAttribute<Rml::String>("name", "");
      if (!name.empty()) {
        out[std::string(name.c_str())] = std::string(control->GetValue().c_str());
      }
    }
  }

  for (int i = 0; i < element->GetNumChildren(); ++i) {
    CollectFormFields(element->GetChild(i), out);
  }
}

void ApplyFormFieldValues(Rml::Element* element, const std::map<std::string, std::string>& values) {
  if (!element) {
    return;
  }

  const Rml::String& tag = element->GetTagName();
  if (tag == "input" || tag == "textarea" || tag == "select") {
    if (auto* control = rmlui_dynamic_cast<Rml::ElementFormControl*>(element)) {
      const Rml::String name = element->GetAttribute<Rml::String>("name", "");
      if (!name.empty()) {
        const auto it = values.find(std::string(name.c_str()));
        if (it != values.end()) {
          control->SetValue(Rml::String(it->second.c_str()));
        }
      }
    }
  }

  for (int i = 0; i < element->GetNumChildren(); ++i) {
    ApplyFormFieldValues(element->GetChild(i), values);
  }
}

Rml::Element* FindFormElement(Rml::Context* context, const std::string& form_element_id) {
  if (!context || context->GetNumDocuments() == 0) {
    return nullptr;
  }
  Rml::ElementDocument* document = context->GetDocument(0);
  if (!document) {
    return nullptr;
  }
  return document->GetElementById(Rml::String(form_element_id.c_str()));
}

} // namespace

std::optional<std::string> ReadFormSubmitTemplate(Rml::Context* context, const std::string& form_element_id) {
  Rml::Element* form = FindFormElement(context, form_element_id);
  if (!form) {
    return std::nullopt;
  }
  const Rml::String template_attr = form->GetAttribute<Rml::String>("data-submit-template", "");
  if (template_attr.empty()) {
    return std::nullopt;
  }
  return std::string(template_attr.c_str());
}

std::map<std::string, std::string> ReadFormValues(Rml::Context* context, const std::string& form_element_id) {
  std::map<std::string, std::string> values;
  Rml::Element* form = FindFormElement(context, form_element_id);
  if (!form) {
    return values;
  }
  CollectFormFields(form, values);
  return values;
}

void RestoreFormValues(Rml::Context* context, const std::string& form_element_id,
                       const std::map<std::string, std::string>& values) {
  if (values.empty()) {
    return;
  }
  Rml::Element* form = FindFormElement(context, form_element_id);
  if (!form) {
    return;
  }
  ApplyFormFieldValues(form, values);
}

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

std::string ExpireFormRml(std::string rml) {
  const std::string expired_class = "class=\"chat-form chat-form-expired\"";
  if (const size_t pos = rml.find("class=\"chat-form\""); pos != std::string::npos) {
    rml.replace(pos, std::char_traits<char>::length("class=\"chat-form\""), expired_class);
  }

  const std::string expired_label = "<p class=\"muted chat-form-expired-label\">Form closed</p>";
  if (const size_t submit_pos = rml.find("class=\"chat-form-submit\""); submit_pos != std::string::npos) {
    const size_t button_start = rml.rfind('<', submit_pos);
    if (button_start != std::string::npos) {
      rml.insert(button_start, expired_label);
    }
  }

  return rml;
}

} // namespace pbr
