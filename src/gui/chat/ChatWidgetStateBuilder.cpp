#include "gui/chat/ChatWidgetStateBuilder.h"

#include <ui/data/DataModelHandle.h>
#include "common/PbrCompat.h"

namespace pbr {

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

void RegisterChatWidgetDataTypes(ui::DataModelConstructor& ctor) {
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

  if (auto message_handle = ctor.RegisterStruct<MessageDisplayRow>()) {
    message_handle.RegisterMember("message_id", &MessageDisplayRow::message_id);
    message_handle.RegisterMember("sender_label", &MessageDisplayRow::sender_label);
    message_handle.RegisterMember("content_rml", &MessageDisplayRow::content_rml);
    message_handle.RegisterMember("row_class", &MessageDisplayRow::row_class);
    message_handle.RegisterMember("transport_badge", &MessageDisplayRow::transport_badge);
    message_handle.RegisterMember("has_content", &MessageDisplayRow::has_content);
    message_handle.RegisterMember("has_form", &MessageDisplayRow::has_form);
    message_handle.RegisterMember("form", &MessageDisplayRow::form);
    message_handle.RegisterMember("has_calendar", &MessageDisplayRow::has_calendar);
    message_handle.RegisterMember("calendar", &MessageDisplayRow::calendar);
  }
  ctor.RegisterArray<std::vector<MessageDisplayRow>>();
}

} // namespace pbr
