#include "domain/ui/ChatWidgetConfigBuilders.h"

#include "domain/ui/CalendarHelper.h"

#include "common/PbrCompat.h"

namespace pbr {

namespace {

ui::String ToRmlString(const std::string& value) {
  return ui::String(value.c_str());
}

} // namespace

FormWidgetState BuildFormWidgetState(const Object& config) {
  FormWidgetState form;
  form.form_id = ToRmlString(config.getString("id").value_or("form"));
  form.title = ToRmlString(config.getString("title").value_or(""));
  form.submit_label = ToRmlString(config.getString("submit_label").value_or("Submit"));
  form.submit_template = ToRmlString(config.getString("submit_template").value_or("Submitted form"));
  form.expired = false;

  if (const Array* fields = config.getArray("fields")) {
    for (const Value& field_value : fields->elements) {
      const Object* field = asObject(field_value);
      if (!field) {
        continue;
      }
      FormFieldRow row;
      row.id = ToRmlString(field->getString("id").value_or(""));
      row.label = ToRmlString(field->getString("label").value_or(row.id.c_str()));
      row.field_type = ToRmlString(field->getString("field_type").value_or("text"));
      row.value = ToRmlString(field->getString("value").value_or(""));

      if (const Array* options = field->getArray("options")) {
        for (const Value& option_value : options->elements) {
          const Object* option = asObject(option_value);
          if (!option) {
            continue;
          }
          FormOptionRow opt;
          opt.label = ToRmlString(option->getString("label").value_or(""));
          opt.value = ToRmlString(option->getString("value").value_or(opt.label.c_str()));
          row.options.push_back(std::move(opt));
        }
      }
      form.fields.push_back(std::move(row));
    }
  }

  return form;
}

CalendarWidgetState BuildCalendarWidgetState(const Object& config) {
  CalendarConfig calendar_config = TodayCalendarConfig();
  if (auto month = config.getIf<int64_t>("month")) {
    calendar_config.month = static_cast<int>(*month);
  }
  if (auto year = config.getIf<int64_t>("year")) {
    calendar_config.year = static_cast<int>(*year);
  }
  calendar_config.min_date = config.getString("min_date").value_or("");
  calendar_config.max_date = config.getString("max_date").value_or("");

  if (const Array* days = config.getArray("available_days")) {
    for (const Value& day : days->elements) {
      if (auto day_text = asString(day)) {
        calendar_config.available_days.push_back(*day_text);
      }
    }
  }

  return BuildCalendarState(calendar_config);
}

} // namespace pbr
