#pragma once

#include <ui/base/Types.h>

#include <cstdint>
#include <string>
#include <vector>

namespace pbr {

struct FormOptionRow {
  ui::String label;
  ui::String value;
};

struct FormFieldRow {
  ui::String id;
  ui::String label;
  ui::String field_type;
  ui::String value;
  bool checked = false;
  std::vector<FormOptionRow> options;
};

struct FormWidgetState {
  ui::String form_id;
  ui::String title;
  ui::String submit_label;
  ui::String submit_template;
  bool expired = false;
  std::vector<FormFieldRow> fields;
};

struct CalendarDayRow {
  int day = 0;
  ui::String label;
  bool available = false;
  bool selected = false;
  ui::String iso_date;
};

struct CalendarWeekRow {
  std::vector<CalendarDayRow> days;
};

struct CalendarWidgetState {
  int month = 1;
  int year = 2000;
  ui::String month_label;
  ui::String min_date;
  ui::String max_date;
  std::vector<ui::String> available_days;
  std::vector<CalendarWeekRow> weeks;
};

struct TranscriptDisplayRow {
  ui::String user_content_rml;
  ui::String assistant_content_rml;
  bool has_assistant = false;
  bool has_form = false;
  FormWidgetState form;
  bool has_calendar = false;
  CalendarWidgetState calendar;
};

struct MessageDisplayRow {
  ui::String message_id;
  ui::String sender_label;
  ui::String content_rml;
  ui::String row_class;
  ui::String transport_badge;
  /** Store cursor for local scroll-up paging (D031) — not bound to RML. */
  int64_t display_order = 0;
  bool has_content = true;
  bool has_form = false;
  FormWidgetState form;
  bool has_calendar = false;
  CalendarWidgetState calendar;
};

struct SessionDisplayRow {
  ui::String id;
  ui::String title;
  ui::String preview;
  /** Visual chat type: ai | private | public | group */
  ui::String kind;
  int unread_count = 0;
  ui::String unread_display;
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
