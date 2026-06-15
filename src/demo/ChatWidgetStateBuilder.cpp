#include "demo/ChatWidgetStateBuilder.h"

#include "demo/CalendarHelper.h"

#include <RmlUi/Core/DataModelHandle.h>

namespace pbr {

namespace {

Rml::String ToRmlString(const std::string& value) {
  return Rml::String(value.c_str());
}

} // namespace

FormWidgetState BuildFormWidgetState(const nlohmann::json& config) {
  FormWidgetState form;
  form.form_id = ToRmlString(config.value("id", "form"));
  form.title = ToRmlString(config.value("title", ""));
  form.submit_label = ToRmlString(config.value("submit_label", "Submit"));
  form.submit_template = ToRmlString(config.value("submit_template", "Submitted form"));
  form.expired = false;

  if (config.contains("fields") && config["fields"].is_array()) {
    for (const auto& field : config["fields"]) {
      if (!field.is_object()) {
        continue;
      }
      FormFieldRow row;
      row.id = ToRmlString(field.value("id", ""));
      row.label = ToRmlString(field.value("label", row.id.c_str()));
      row.field_type = ToRmlString(field.value("field_type", "text"));
      row.value = ToRmlString(field.value("value", ""));

      if (field.contains("options") && field["options"].is_array()) {
        for (const auto& option : field["options"]) {
          if (!option.is_object()) {
            continue;
          }
          FormOptionRow opt;
          opt.label = ToRmlString(option.value("label", ""));
          opt.value = ToRmlString(option.value("value", opt.label.c_str()));
          row.options.push_back(std::move(opt));
        }
      }
      form.fields.push_back(std::move(row));
    }
  }

  return form;
}

CalendarWidgetState BuildCalendarWidgetState(const nlohmann::json& config) {
  CalendarConfig calendar_config = TodayCalendarConfig();
  if (config.contains("month") && config["month"].is_number_integer()) {
    calendar_config.month = config["month"].get<int>();
  }
  if (config.contains("year") && config["year"].is_number_integer()) {
    calendar_config.year = config["year"].get<int>();
  }
  calendar_config.min_date = config.value("min_date", "");
  calendar_config.max_date = config.value("max_date", "");

  if (config.contains("available_days") && config["available_days"].is_array()) {
    for (const auto& day : config["available_days"]) {
      if (day.is_string()) {
        calendar_config.available_days.push_back(day.get<std::string>());
      }
    }
  }

  return BuildCalendarState(calendar_config);
}

void ApplyWidgetInits(const std::vector<WidgetInit>& inits, TurnWidgetState& state) {
  for (const WidgetInit& init : inits) {
    if (init.kind == WidgetInitKind::Form) {
      state.has_form = true;
      state.form = BuildFormWidgetState(init.config);
    } else if (init.kind == WidgetInitKind::Calendar) {
      state.has_calendar = true;
      state.calendar = BuildCalendarWidgetState(init.config);
    }
  }
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

void RegisterChatWidgetDataTypes(Rml::DataModelConstructor& ctor) {
  if (auto option_handle = ctor.RegisterStruct<FormOptionRow>()) {
    option_handle.RegisterMember("label", &FormOptionRow::label);
    option_handle.RegisterMember("value", &FormOptionRow::value);
  }
  ctor.RegisterArray<std::vector<FormOptionRow>>();

  if (auto field_handle = ctor.RegisterStruct<FormFieldRow>()) {
    field_handle.RegisterMember("id", &FormFieldRow::id);
    field_handle.RegisterMember("label", &FormFieldRow::label);
    field_handle.RegisterMember("field_type", &FormFieldRow::field_type);
    field_handle.RegisterMember("value", &FormFieldRow::value);
    field_handle.RegisterMember("checked", &FormFieldRow::checked);
    field_handle.RegisterMember("options", &FormFieldRow::options);
  }
  ctor.RegisterArray<std::vector<FormFieldRow>>();

  if (auto form_handle = ctor.RegisterStruct<FormWidgetState>()) {
    form_handle.RegisterMember("form_id", &FormWidgetState::form_id);
    form_handle.RegisterMember("title", &FormWidgetState::title);
    form_handle.RegisterMember("submit_label", &FormWidgetState::submit_label);
    form_handle.RegisterMember("submit_template", &FormWidgetState::submit_template);
    form_handle.RegisterMember("expired", &FormWidgetState::expired);
    form_handle.RegisterMember("fields", &FormWidgetState::fields);
  }

  if (auto day_handle = ctor.RegisterStruct<CalendarDayRow>()) {
    day_handle.RegisterMember("day", &CalendarDayRow::day);
    day_handle.RegisterMember("label", &CalendarDayRow::label);
    day_handle.RegisterMember("available", &CalendarDayRow::available);
    day_handle.RegisterMember("selected", &CalendarDayRow::selected);
    day_handle.RegisterMember("iso_date", &CalendarDayRow::iso_date);
  }
  ctor.RegisterArray<std::vector<CalendarDayRow>>();

  if (auto week_handle = ctor.RegisterStruct<CalendarWeekRow>()) {
    week_handle.RegisterMember("days", &CalendarWeekRow::days);
  }
  ctor.RegisterArray<std::vector<CalendarWeekRow>>();

  if (auto calendar_handle = ctor.RegisterStruct<CalendarWidgetState>()) {
    calendar_handle.RegisterMember("month", &CalendarWidgetState::month);
    calendar_handle.RegisterMember("year", &CalendarWidgetState::year);
    calendar_handle.RegisterMember("month_label", &CalendarWidgetState::month_label);
    calendar_handle.RegisterMember("min_date", &CalendarWidgetState::min_date);
    calendar_handle.RegisterMember("max_date", &CalendarWidgetState::max_date);
    calendar_handle.RegisterMember("weeks", &CalendarWidgetState::weeks);
  }

  if (auto turn_handle = ctor.RegisterStruct<TranscriptDisplayRow>()) {
    turn_handle.RegisterMember("user_content_rml", &TranscriptDisplayRow::user_content_rml);
    turn_handle.RegisterMember("assistant_content_rml", &TranscriptDisplayRow::assistant_content_rml);
    turn_handle.RegisterMember("has_assistant", &TranscriptDisplayRow::has_assistant);
    turn_handle.RegisterMember("has_form", &TranscriptDisplayRow::has_form);
    turn_handle.RegisterMember("form", &TranscriptDisplayRow::form);
    turn_handle.RegisterMember("has_calendar", &TranscriptDisplayRow::has_calendar);
    turn_handle.RegisterMember("calendar", &TranscriptDisplayRow::calendar);
  }
  ctor.RegisterArray<std::vector<TranscriptDisplayRow>>();
}

} // namespace pbr
