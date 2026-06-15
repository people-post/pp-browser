#include "demo/ChatFormHelper.h"

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

  std::string expired = pbr::ExpireFormRml(R"(<div class="chat-form"><button class="chat-form-submit"></button></div>)");
  assert(expired.find("chat-form-expired") != std::string::npos);
  assert(expired.find("Form closed") != std::string::npos);

  std::cout << "chat_form_helper_test passed\n";
  return 0;
}
