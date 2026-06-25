#pragma once

#include <RmlUi/Core/Types.h>

#include <string>
#include <vector>

namespace pbr {

struct FormOptionRow {
  Rml::String label;
  Rml::String value;
};

struct FormFieldRow {
  Rml::String id;
  Rml::String label;
  Rml::String field_type;
  Rml::String value;
  bool checked = false;
  std::vector<FormOptionRow> options;
};

struct FormWidgetState {
  Rml::String form_id;
  Rml::String title;
  Rml::String submit_label;
  Rml::String submit_template;
  bool expired = false;
  std::vector<FormFieldRow> fields;
};

struct CalendarDayRow {
  int day = 0;
  Rml::String label;
  bool available = false;
  bool selected = false;
  Rml::String iso_date;
};

struct CalendarWeekRow {
  std::vector<CalendarDayRow> days;
};

struct CalendarWidgetState {
  int month = 1;
  int year = 2000;
  Rml::String month_label;
  Rml::String min_date;
  Rml::String max_date;
  std::vector<Rml::String> available_days;
  std::vector<CalendarWeekRow> weeks;
};

struct TranscriptDisplayRow {
  Rml::String user_content_rml;
  Rml::String assistant_content_rml;
  bool has_assistant = false;
  bool has_form = false;
  FormWidgetState form;
  bool has_calendar = false;
  CalendarWidgetState calendar;
};

struct MessageDisplayRow {
  Rml::String sender_label;
  Rml::String content_rml;
  Rml::String row_class;
  bool has_content = true;
  bool has_form = false;
  FormWidgetState form;
  bool has_calendar = false;
  CalendarWidgetState calendar;
};

struct SessionDisplayRow {
  Rml::String id;
  Rml::String title;
  Rml::String preview;
  Rml::String kind;
  int unread_count = 0;
  bool active = false;
  bool closable = false;
};

struct TurnWidgetState {
  bool has_form = false;
  FormWidgetState form;
  bool has_calendar = false;
  CalendarWidgetState calendar;
};

} // namespace pbr
