#pragma once

#include <ui/base/Types.h>

#include <cstdint>
#include <optional>
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
  /** A peer's message mentions this account (group-chat G015): the whole row is tinted. */
  bool mentions_me = false;
  /** The message has reaction chips: drawn over its bubble's bottom edge, so the row keeps room below for them. */
  bool has_reactions = false;
  /** Search inside the chat: the row contains the query, and the row the search is on (its matches are marked). */
  bool search_hit = false;
  bool search_current = false;
  /** content_rml as built, while search marks are wrapped into it — not bound to RML. */
  std::optional<std::string> search_source_rml;
  /** Local send / receive time, e.g. "18:32"; empty when unknown. */
  ui::String time_label;
  /** Phone: the centred date strip above this row, set when its day differs from the row before; else empty. */
  ui::String date_label;
  /** Phone: this row starts a run (a different sender, a gap over 5 minutes, a day change, or a non-message row before). */
  bool run_start = true;
  /** Phone: the last bubble of a run (carries the bubble tail and, in groups, the avatar). */
  bool run_end = false;
  /** Phone, group chats: a peer's name above the first bubble of their run, in their avatar colour; else empty. */
  ui::String group_name;
  /** Index of the avatar colour (0 .. kAvatarToneCount-1) for `group_name` (same input as the Contacts list: name only). */
  int group_name_tone = 0;
  /** Phone, group chats: `group_name` is written inside the bubble (not as its own line above it). */
  bool group_name_in_bubble = false;
  /** Phone, group chats: every received message row keeps a 32dp avatar column on the left. */
  bool group_column = false;
  /** Phone, group chats: the column shows the avatar (last bubble of a run only); `group_avatar_letter` is its letter. */
  bool group_avatar = false;
  ui::String group_avatar_letter;
  /** The sender's own cached profile photo (local file) in the avatar column; empty: the letter shows. */
  ui::String group_avatar_src;
  bool group_has_icon = false;
  /** Store cursor for local scroll-up paging (D031) — not bound to RML. */
  int64_t display_order = 0;
  /** `ThreadMessage::ai_image_json` of an AI question that carried an image — not bound to RML. */
  std::string ai_image_json;
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
  /** When the session was last active; format rules in feature/conversations/SessionListText.h. */
  ui::String date_label;
  bool active = false;
  /** Per-thread local settings (pin / mute); archived rows are listed separately. */
  bool pinned = false;
  bool muted = false;
  /** Marked unread by hand and no real unread count: show a dot, not a number. */
  bool unread_dot = false;
  /** An unread message here mentions us (group-chat G015): the preview line leads with "You were mentioned". */
  bool mentioned = false;
  /** Round avatar: first letter of the title and the colour index (avatar-tone-0..7). */
  ui::String avatar_letter;
  int avatar_tone = 0;
  /** The person's own profile photo (local file) when cached; else the letter shows. */
  bool has_icon = false;
  ui::String icon_src;
};

struct TurnWidgetState {
  bool has_form = false;
  FormWidgetState form;
  bool has_calendar = false;
  CalendarWidgetState calendar;
};

} // namespace pbr
