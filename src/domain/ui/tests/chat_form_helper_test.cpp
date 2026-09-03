#include "domain/ui/ChatFormHelper.h"

#include <gtest/gtest.h>

#include <map>
#include <optional>
#include <string>

TEST(ChatFormHelperTest, AppliesSubmitTemplate) {
  const std::map<std::string, std::string> values{{"name", "Alice"}, {"date", "2026-06-15"}};
  const std::string display =
      pbr::ApplySubmitTemplate("Book for {{name}} on {{date}}", values);
  EXPECT_EQ(display, "Book for Alice on 2026-06-15");
}

TEST(ChatFormHelperTest, BuildsFormSubmissionPayload) {
  const std::map<std::string, std::string> values{{"name", "Alice"}, {"date", "2026-06-15"}};
  const std::string payload = pbr::BuildFormSubmissionPayload("booking", values);
  EXPECT_NE(payload.find("\"type\":\"form_submission\""), std::string::npos);
  EXPECT_NE(payload.find("\"form_id\":\"booking\""), std::string::npos);
  EXPECT_NE(payload.find("\"name\":\"Alice\""), std::string::npos);
}

TEST(ChatFormHelperTest, ExtractsFormIdAndInjectsPlaceholders) {
  const std::string rml =
      R"(<div class="chat-form" data-form-id="booking" id="form-__ENTRY__-booking"></div>)";
  const std::optional<std::string> form_id = pbr::ExtractFormId(rml);
  ASSERT_TRUE(form_id.has_value());
  EXPECT_EQ(*form_id, "booking");

  const std::string hydrated = pbr::InjectEntryPlaceholders(rml, "entry_42");
  EXPECT_EQ(hydrated.find("__ENTRY__"), std::string::npos);
  EXPECT_NE(hydrated.find("entry_42"), std::string::npos);
}

TEST(ChatFormHelperTest, BuildsFormValuesMap) {
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
  EXPECT_EQ(bound_values.at("name"), "Alice");
  EXPECT_EQ(bound_values.at("date"), "2026-06-15");
}
