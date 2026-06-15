#include "demo/ChatFormHelper.h"
#include "demo/ChatWidgetStateBuilder.h"

#include <cassert>
#include <iostream>
#include <map>
#include <optional>
#include <string>

int main() {
  const std::map<std::string, std::string> values{{"name", "Alice"}, {"date", "2026-06-15"}};
  const std::string display =
      pbr::ApplySubmitTemplate("Book for {{name}} on {{date}}", values);
  assert(display == "Book for Alice on 2026-06-15");

  const std::string payload = pbr::BuildFormSubmissionPayload("booking", values);
  assert(payload.find("\"type\":\"form_submission\"") != std::string::npos);
  assert(payload.find("\"form_id\":\"booking\"") != std::string::npos);
  assert(payload.find("\"name\":\"Alice\"") != std::string::npos);

  const std::string rml = R"(<div class="chat-form" data-form-id="booking" id="form-__ENTRY__-booking"></div>)";
  const std::optional<std::string> form_id = pbr::ExtractFormId(rml);
  assert(form_id.has_value());
  assert(*form_id == "booking");

  const std::string hydrated = pbr::InjectEntryPlaceholders(rml, "entry_42");
  assert(hydrated.find("__ENTRY__") == std::string::npos);
  assert(hydrated.find("entry_42") != std::string::npos);

  pbr::FormWidgetState form;
  form.form_id = "booking";
  pbr::FormFieldRow name_field;
  name_field.id = "name";
  name_field.value = "Alice";
  form.fields.push_back(name_field);
  pbr::FormFieldRow date_field;
  date_field.id = "date";
  date_field.value = "2026-06-15";
  form.fields.push_back(date_field);

  const std::map<std::string, std::string> bound_values = pbr::FormValuesMap(form);
  assert(bound_values.at("name") == "Alice");
  assert(bound_values.at("date") == "2026-06-15");

  std::cout << "chat_form_helper_test passed\n";
  return 0;
}
