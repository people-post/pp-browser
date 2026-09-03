#include "domain/ai/StructuredTextParser.h"

#include "domain/ai/WorkingSetPolicy.h"
#include "common/chat/PeopleDiscoveryBlocks.h"

#include "common/ValueJson.h"
#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>
#include "common/PbrCompat.h"

namespace pbr {

std::string StructuredTextParser::EscapeText(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    switch (c) {
    case '&':
      out += "&amp;";
      break;
    case '<':
      out += "&lt;";
      break;
    case '>':
      out += "&gt;";
      break;
    case '"':
      out += "&quot;";
      break;
    default:
      out += c;
      break;
    }
  }
  return out;
}

std::string StructuredTextParser::EscapeExpressionString(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    if (c == '\\' || c == '\'') {
      out += '\\';
    }
    out += c;
  }
  return out;
}

namespace {

std::string TrimAsciiWhitespace(std::string text) {
  const auto not_space = [](unsigned char c) { return !std::isspace(c); };
  text.erase(text.begin(), std::find_if(text.begin(), text.end(), not_space));
  text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(), text.end());
  return text;
}

std::string BalanceJsonBraces(std::string json) {
  int depth = 0;
  for (char c : json) {
    if (c == '{')
      depth++;
    else if (c == '}')
      depth = std::max(0, depth - 1);
  }
  while (depth-- > 0)
    json += '}';
  return json;
}

ParseResult Fail(const std::string& message) {
  ParseResult result;
  result.ok = false;
  result.error = message;
  result.rml = "<p class=\"error\">" + StructuredTextParser::EscapeText(message) + "</p>";
  return result;
}

ParseResult BlockError(const std::string& message) {
  ParseResult result;
  result.ok = false;
  result.error = message;
  return result;
}

constexpr const char* kFormWidgetRml =
    "<p class=\"muted chat-form-expired-label\" data-if=\"turn.has_form && turn.form.expired\">Form closed</p>"
    "<div class=\"chat-form\" data-form-id=\"__FORM_ID__\" data-if=\"turn.has_form && !turn.form.expired\">"
    "<p class=\"muted\">{{turn.form.title}}</p>"
    "<div data-for=\"field : turn.form.fields\">"
    "<label class=\"chat-form-label\">{{field.label}}"
    "<input class=\"chat-form-field\" type=\"text\" data-if=\"field.field_type == 'text'\" data-value=\"field.value\" />"
    "<input class=\"chat-form-field\" type=\"text\" data-if=\"field.field_type == 'date'\" data-value=\"field.value\" "
    "placeholder=\"YYYY-MM-DD\" />"
    "<textarea class=\"chat-form-field\" rows=\"2\" data-if=\"field.field_type == 'textarea'\" data-value=\"field.value\"></textarea>"
    "<select class=\"chat-form-field\" data-if=\"field.field_type == 'select'\" data-value=\"field.value\">"
    "<option data-for=\"option : field.options\" data-value=\"option.value\">{{option.label}}</option>"
    "</select>"
    "<input type=\"checkbox\" data-if=\"field.field_type == 'checkbox'\" data-checked=\"field.checked\" />"
    "</label>"
    "</div>"
    "<button type=\"button\" class=\"chat-form-submit\" "
    "data-event-click=\"submit_form('__ENTRY__', '__FORM_ID__')\">{{turn.form.submit_label}}</button>"
    "</div>";

constexpr const char* kFormPanelWidgetRml =
    "<p class=\"muted chat-form-expired-label\" data-if=\"working_set.has_form && working_set.form.expired\">Form closed</p>"
    "<div class=\"working-set-form chat-form\" data-form-id=\"__FORM_ID__\" data-if=\"working_set.has_form && !working_set.form.expired\">"
    "<p class=\"muted\">{{working_set.form.title}}</p>"
    "<div data-for=\"field : working_set.form.fields\">"
    "<label class=\"chat-form-label\">{{field.label}}"
    "<input class=\"chat-form-field\" type=\"text\" data-if=\"field.field_type == 'text'\" data-value=\"field.value\" />"
    "<input class=\"chat-form-field\" type=\"text\" data-if=\"field.field_type == 'date'\" data-value=\"field.value\" "
    "placeholder=\"YYYY-MM-DD\" />"
    "<textarea class=\"chat-form-field\" rows=\"2\" data-if=\"field.field_type == 'textarea'\" data-value=\"field.value\"></textarea>"
    "<select class=\"chat-form-field\" data-if=\"field.field_type == 'select'\" data-value=\"field.value\">"
    "<option data-for=\"option : field.options\" data-value=\"option.value\">{{option.label}}</option>"
    "</select>"
    "<input type=\"checkbox\" data-if=\"field.field_type == 'checkbox'\" data-checked=\"field.checked\" />"
    "</label>"
    "</div>"
    "<button type=\"button\" class=\"chat-form-submit\" "
    "data-event-click=\"submit_form('__ENTRY__', '__FORM_ID__')\">{{working_set.form.submit_label}}</button>"
    "</div>";

constexpr const char* kCalendarPanelWidgetRml =
    "<div class=\"working-set-calendar chat-calendar\" data-if=\"working_set.has_calendar\">"
    "<div class=\"row calendar-header\">"
    "<button type=\"button\" class=\"calendar-nav\" data-event-click=\"calendar_prev('__ENTRY__')\">&lt;</button>"
    "<span>{{working_set.calendar.month_label}}</span>"
    "<button type=\"button\" class=\"calendar-nav\" data-event-click=\"calendar_next('__ENTRY__')\">&gt;</button>"
    "</div>"
    "<div class=\"calendar-weekdays row\">"
    "<span>Su</span><span>Mo</span><span>Tu</span><span>We</span><span>Th</span><span>Fr</span><span>Sa</span>"
    "</div>"
    "<table class=\"calendar-grid\">"
    "<tr data-for=\"week : working_set.calendar.weeks\">"
    "<td class=\"calendar-cell\" data-for=\"day : week.days\">"
    "<button type=\"button\" class=\"calendar-day\" data-if=\"day.available\" "
    "data-event-click=\"select_calendar_day('__ENTRY__', day.iso_date)\">{{day.label}}</button>"
    "<span class=\"calendar-day calendar-day-muted\" data-if=\"!day.available\">{{day.label}}</span>"
    "</td>"
    "</tr>"
    "</table>"
    "</div>";

constexpr const char* kCalendarWidgetRml =
    "<div class=\"chat-calendar\" data-if=\"turn.has_calendar\">"
    "<div class=\"row calendar-header\">"
    "<button type=\"button\" class=\"calendar-nav\" data-event-click=\"calendar_prev('__ENTRY__')\">&lt;</button>"
    "<span>{{turn.calendar.month_label}}</span>"
    "<button type=\"button\" class=\"calendar-nav\" data-event-click=\"calendar_next('__ENTRY__')\">&gt;</button>"
    "</div>"
    "<div class=\"calendar-weekdays row\">"
    "<span>Su</span><span>Mo</span><span>Tu</span><span>We</span><span>Th</span><span>Fr</span><span>Sa</span>"
    "</div>"
    "<table class=\"calendar-grid\">"
    "<tr data-for=\"week : turn.calendar.weeks\">"
    "<td class=\"calendar-cell\" data-for=\"day : week.days\">"
    "<button type=\"button\" class=\"calendar-day\" data-if=\"day.available\" "
    "data-event-click=\"select_calendar_day('__ENTRY__', day.iso_date)\">{{day.label}}</button>"
    "<span class=\"calendar-day calendar-day-muted\" data-if=\"!day.available\">{{day.label}}</span>"
    "</td>"
    "</tr>"
    "</table>"
    "</div>";

std::string ReplaceAll(std::string text, const std::string& from, const std::string& to) {
  size_t pos = 0;
  while ((pos = text.find(from, pos)) != std::string::npos) {
    text.replace(pos, from.size(), to);
    pos += to.size();
  }
  return text;
}

std::optional<std::string> ParseOptionalButtonPayload(const Object& block) {
  auto payload_slot = block.fields().tryGet("payload");
  if (!payload_slot) {
    return std::nullopt;
  }
  const Value& payload = payload_slot->get();
  if (const Object* payload_obj = asObject(payload)) {
    return DumpJson(*payload_obj);
  }
  auto payload_str = asString(payload);
  if (!payload_str || payload_str->empty()) {
    return std::nullopt;
  }
  if (!TryParseObject(*payload_str)) {
    return std::nullopt;
  }
  return payload_str;
}

ParseResult AppendChatActionButton(ParseResult& parent, const std::string& label, const std::string& message,
                                   const std::optional<std::string>& payload) {
  if (message.empty()) {
    return BlockError("button message must not be empty");
  }

  const int index = static_cast<int>(parent.chat_actions.size());
  parent.chat_actions.push_back({label, message, payload});

  ParseResult result;
  result.ok = true;
  result.rml = "<button class=\"chat-suggestion\" data-event-click=\"send_chat_action('__ENTRY__', " +
               std::to_string(index) + ")\">" + StructuredTextParser::EscapeText(label) + "</button>";
  return result;
}

ParseResult ParseButtonBlock(const Object& block, ParseResult& parent) {
  if (!block.getString("label")) {
    return BlockError("button block requires label");
  }
  if (!block.getString("message")) {
    return BlockError("button block requires message");
  }

  std::optional<std::string> payload;
  if (block.contains("payload")) {
    payload = ParseOptionalButtonPayload(block);
    if (!payload) {
      return BlockError("button payload must be a JSON object or object string");
    }
  }

  return AppendChatActionButton(parent, *block.getString("label"), *block.getString("message"),
                                payload);
}

ParseResult ParseCardBlock(const Object& block) {
  if (!block.getString("title")) {
    return BlockError("card block requires title");
  }
  if (!block.getString("body")) {
    return BlockError("card block requires body");
  }

  const std::string variant = block.getString("variant").value_or("default");
  std::ostringstream out;
  out << "<div class=\"chat-card chat-card-" << StructuredTextParser::EscapeText(variant) << "\">";
  out << "<h3>" << StructuredTextParser::EscapeText(*block.getString("title")) << "</h3>";
  if (block.getString("subtitle")) {
    out << "<p class=\"muted\">" << StructuredTextParser::EscapeText(*block.getString("subtitle")) << "</p>";
  }
  out << "<p>" << StructuredTextParser::EscapeText(*block.getString("body")) << "</p>";
  out << "</div>";

  ParseResult result;
  result.ok = true;
  result.rml = out.str();
  return result;
}

ParseResult ParseTableBlock(const Object& block) {
  if (!block.getArray("headers")) {
    return BlockError("table block requires headers array");
  }
  if (!block.getArray("rows")) {
    return BlockError("table block requires rows array");
  }

  std::ostringstream out;
  out << "<table class=\"chat-table\"><thead><tr>";
  for (const Value& header : block.getArray("headers")->elements) {
    auto header_text = asString(header);
    if (!header_text) {
      return BlockError("table headers must be strings");
    }
    out << "<th>" << StructuredTextParser::EscapeText(*header_text) << "</th>";
  }
  out << "</tr></thead><tbody>";
  for (const Value& row : block.getArray("rows")->elements) {
    const Array* cells = asArray(row);
    if (!cells) {
      return BlockError("table rows must be arrays");
    }
    out << "<tr>";
    for (const Value& cell : cells->elements) {
      auto cell_text = asString(cell);
      if (!cell_text) {
        return BlockError("table cells must be strings");
      }
      out << "<td>" << StructuredTextParser::EscapeText(*cell_text) << "</td>";
    }
    out << "</tr>";
  }
  out << "</tbody></table>";

  ParseResult result;
  result.ok = true;
  result.rml = out.str();
  return result;
}

ParseResult ParseKeyValueBlock(const Object& block) {
  if (!block.getArray("items")) {
    return BlockError("key_value block requires items array");
  }

  std::ostringstream out;
  out << "<div class=\"chat-key-value\">";
  for (const Value& item_value : block.getArray("items")->elements) {
    const Object* item = asObject(item_value);
    if (!item || !item->getString("label") || !item->getString("value")) {
      return BlockError("key_value items require label and value");
    }
    out << "<div class=\"chat-key-value-row\">";
    out << "<span class=\"chat-key-value-label\">" << StructuredTextParser::EscapeText(*item->getString("label"))
        << "</span>";
    out << "<span class=\"chat-key-value-value\">" << StructuredTextParser::EscapeText(*item->getString("value"))
        << "</span>";
    out << "</div>";
  }
  out << "</div>";

  ParseResult result;
  result.ok = true;
  result.rml = out.str();
  return result;
}

ParseResult ParseCalloutBlock(const Object& block) {
  if (!block.getString("text")) {
    return BlockError("callout block requires text");
  }
  const std::string variant = block.getString("variant").value_or("info");
  ParseResult result;
  result.ok = true;
  result.rml = "<div class=\"chat-callout chat-callout-" + StructuredTextParser::EscapeText(variant) + "\"><p>" +
               StructuredTextParser::EscapeText(*block.getString("text")) + "</p></div>";
  return result;
}

ParseResult ParseQuoteBlock(const Object& block) {
  if (!block.getString("text")) {
    return BlockError("quote block requires text");
  }
  std::ostringstream out;
  out << "<blockquote class=\"chat-quote\"><p>" << StructuredTextParser::EscapeText(*block.getString("text"))
      << "</p>";
  if (block.getString("attribution")) {
    out << "<p class=\"muted\">— " << StructuredTextParser::EscapeText(*block.getString("attribution")) << "</p>";
  }
  out << "</blockquote>";

  ParseResult result;
  result.ok = true;
  result.rml = out.str();
  return result;
}

ParseResult ParseFormBlock(const Object& block) {
  if (!block.getString("id")) {
    return BlockError("form block requires id");
  }
  if (!block.getArray("fields")) {
    return BlockError("form block requires fields array");
  }
  if (!block.getString("submit_template")) {
    return BlockError("form block requires submit_template");
  }

  for (const Value& field_value : block.getArray("fields")->elements) {
    const Object* field = asObject(field_value);
    if (!field || !field->getString("id")) {
      return BlockError("form fields require id");
    }
    if (!field->getString("label")) {
      return BlockError("form fields require label");
    }
  }

  const std::string form_id = *block.getString("id");
  ParseResult result;
  result.ok = true;
  result.rml = ReplaceAll(ReplaceAll(kFormWidgetRml, "__FORM_ID__", form_id), "__ENTRY__", "__ENTRY__");
  result.widget_inits.push_back({WidgetInitKind::Form, block});
  return result;
}

ParseResult ParseCalendarBlock(const Object& block) {
  if (block.contains("month") && !block.getIf<int64_t>("month")) {
    return BlockError("calendar month must be an integer");
  }
  if (block.contains("year") && !block.getIf<int64_t>("year")) {
    return BlockError("calendar year must be an integer");
  }

  ParseResult result;
  result.ok = true;
  result.rml = kCalendarWidgetRml;
  result.widget_inits.push_back({WidgetInitKind::Calendar, block});
  return result;
}

ParseResult ParseLongListActionButton(ParseResult& parent, const Value& action_value) {
  const Object* action = asObject(action_value);
  if (!action || !action->getString("label") || !action->getString("message")) {
    return BlockError("long_list actions require label and message");
  }
  if (action->contains("payload")) {
    const auto payload = ParseOptionalButtonPayload(*action);
    if (!payload) {
      return BlockError("long_list action payload must be a JSON object or object string");
    }
    return AppendChatActionButton(parent, *action->getString("label"), *action->getString("message"),
                                  payload);
  }
  return AppendChatActionButton(parent, *action->getString("label"), *action->getString("message"),
                                std::nullopt);
}

ParseResult ParseLongListBlock(const Object& block, ParseResult& parent) {
  if (!block.getArray("items")) {
    return BlockError("long_list block requires items array");
  }

  std::ostringstream out;
  out << "<div class=\"chat-long-list\">";
  if (block.getString("title")) {
    out << "<p class=\"muted\">" << StructuredTextParser::EscapeText(*block.getString("title")) << "</p>";
  }
  out << "<div class=\"chat-long-list-scroll\">";
  for (const Value& item_value : block.getArray("items")->elements) {
    const Object* item = asObject(item_value);
    if (!item || !item->getString("title")) {
      return BlockError("long_list items require title");
    }
    out << "<div class=\"chat-long-list-item\">";
    out << "<p class=\"chat-long-list-title\">" << StructuredTextParser::EscapeText(*item->getString("title"))
        << "</p>";
    if (auto subtitle = item->getString("subtitle")) {
      out << "<p class=\"muted chat-long-list-subtitle\">"
          << StructuredTextParser::EscapeText(*subtitle) << "</p>";
    }
    if (auto meta = item->getString("meta")) {
      out << "<p class=\"muted chat-long-list-meta\">" << StructuredTextParser::EscapeText(*meta)
          << "</p>";
    }
    if (const Array* actions = item->getArray("actions")) {
      out << "<div class=\"row chat-long-list-actions\">";
      for (const Value& action_value : actions->elements) {
        auto button = ParseLongListActionButton(parent, action_value);
        if (!button.ok) {
          return button;
        }
        out << button.rml;
      }
      out << "</div>";
    }
    out << "</div>";
  }
  out << "</div>";
  if (block.getArray("footer_actions")) {
    out << "<div class=\"row chat-long-list-footer\">";
    for (const Value& action_value : block.getArray("footer_actions")->elements) {
      auto button = ParseLongListActionButton(parent, action_value);
      if (!button.ok) {
        return button;
      }
      out << button.rml;
    }
    out << "</div>";
  }
  out << "</div>";

  ParseResult result;
  result.ok = true;
  result.rml = out.str();
  return result;
}

ParseResult ParseLongListArtifact(const Object& block, ParseResult& parent) {
  if (!block.getArray("items")) {
    return BlockError("long_list block requires items array");
  }

  std::ostringstream out;
  out << "<div class=\"working-set-long-list\">";
  if (block.getString("title")) {
    out << "<p class=\"muted\">" << StructuredTextParser::EscapeText(*block.getString("title")) << "</p>";
  }
  out << "<div class=\"working-set-long-list-body\">";
  for (const Value& item_value : block.getArray("items")->elements) {
    const Object* item = asObject(item_value);
    if (!item || !item->getString("title")) {
      return BlockError("long_list items require title");
    }
    out << "<div class=\"chat-long-list-item\">";
    out << "<p class=\"chat-long-list-title\">" << StructuredTextParser::EscapeText(*item->getString("title"))
        << "</p>";
    if (auto subtitle = item->getString("subtitle")) {
      out << "<p class=\"muted chat-long-list-subtitle\">"
          << StructuredTextParser::EscapeText(*subtitle) << "</p>";
    }
    if (auto meta = item->getString("meta")) {
      out << "<p class=\"muted chat-long-list-meta\">" << StructuredTextParser::EscapeText(*meta)
          << "</p>";
    }
    if (const Array* actions = item->getArray("actions")) {
      out << "<div class=\"row chat-long-list-actions\">";
      for (const Value& action_value : actions->elements) {
        auto button = ParseLongListActionButton(parent, action_value);
        if (!button.ok) {
          return button;
        }
        out << button.rml;
      }
      out << "</div>";
    }
    out << "</div>";
  }
  out << "</div>";
  if (block.getArray("footer_actions")) {
    out << "<div class=\"row chat-long-list-footer\">";
    for (const Value& action_value : block.getArray("footer_actions")->elements) {
      auto button = ParseLongListActionButton(parent, action_value);
      if (!button.ok) {
        return button;
      }
      out << button.rml;
    }
    out << "</div>";
  }
  out << "</div>";

  ParseResult result;
  result.ok = true;
  result.rml = out.str();
  return result;
}

std::string BuildArtifactRml(const Object& block, const WorkingSetKind kind, const std::string& inline_rml,
                             ParseResult& parent) {
  switch (kind) {
  case WorkingSetKind::LongList: {
    const auto artifact = ParseLongListArtifact(block, parent);
    return artifact.ok ? artifact.rml : inline_rml;
  }
  case WorkingSetKind::Form: {
    const std::string form_id = *block.getString("id");
    return ReplaceAll(ReplaceAll(kFormPanelWidgetRml, "__FORM_ID__", form_id), "__ENTRY__", "__ENTRY__");
  }
  case WorkingSetKind::Calendar:
    return kCalendarPanelWidgetRml;
  case WorkingSetKind::Table:
    return ReplaceAll(inline_rml, "chat-table", "working-set-table chat-table");
  case WorkingSetKind::Code:
    return ReplaceAll(inline_rml, "code-block", "working-set-code code-block");
  case WorkingSetKind::KeyValue:
    return ReplaceAll(inline_rml, "chat-key-value", "working-set-key-value chat-key-value");
  case WorkingSetKind::Card:
    return ReplaceAll(inline_rml, "chat-card", "working-set-card chat-card");
  default:
    return inline_rml;
  }
}

ParseResult ParseActionListBlock(const Object& block, ParseResult& parent) {
  if (!block.getArray("items")) {
    return BlockError("action_list block requires items array");
  }

  std::ostringstream out;
  out << "<div class=\"chat-action-list\">";
  for (const Value& item_value : block.getArray("items")->elements) {
    const Object* item = asObject(item_value);
    if (!item || !item->getString("title")) {
      return BlockError("action_list items require title");
    }
    out << "<div class=\"chat-action-list-item\">";
    out << "<p class=\"chat-action-list-title\">" << StructuredTextParser::EscapeText(*item->getString("title"))
        << "</p>";
    if (auto description = item->getString("description")) {
      out << "<p class=\"muted\">" << StructuredTextParser::EscapeText(*description) << "</p>";
    }
    if (const Array* actions = item->getArray("actions")) {
      out << "<div class=\"row chat-action-list-actions\">";
      for (const Value& action_value : actions->elements) {
        const Object* action = asObject(action_value);
        if (!action || !action->getString("label") || !action->getString("message")) {
          return BlockError("action_list actions require label and message");
        }
        auto button = AppendChatActionButton(parent, *action->getString("label"),
                                               *action->getString("message"),
                                               action->contains("payload") ? ParseOptionalButtonPayload(*action)
                                                                          : std::nullopt);
        if (!button.ok) {
          return button;
        }
        out << button.rml;
      }
      out << "</div>";
    }
    out << "</div>";
  }
  out << "</div>";

  ParseResult result;
  result.ok = true;
  result.rml = out.str();
  return result;
}

ParseResult ParseChoiceBlock(const Object& block, ParseResult& parent) {
  if (!block.getString("prompt")) {
    return BlockError("choice block requires prompt");
  }
  if (!block.getArray("options")) {
    return BlockError("choice block requires options array");
  }

  std::ostringstream out;
  out << "<div class=\"chat-choice\"><p>" << StructuredTextParser::EscapeText(*block.getString("prompt"))
      << "</p><div class=\"row chat-choice-options\">";
  for (const Value& option_value : block.getArray("options")->elements) {
    const Object* option = asObject(option_value);
    if (!option || !option->getString("label") || !option->getString("message")) {
      return BlockError("choice options require label and message");
    }
    auto button = AppendChatActionButton(parent, *option->getString("label"), *option->getString("message"),
                                         option->contains("payload") ? ParseOptionalButtonPayload(*option)
                                                                    : std::nullopt);
    if (!button.ok) {
      return button;
    }
    out << button.rml;
  }
  out << "</div></div>";

  ParseResult result;
  result.ok = true;
  result.rml = out.str();
  return result;
}

ParseResult ParsePollBlock(const Object& block, ParseResult& parent) {
  if (!block.getString("question")) {
    return BlockError("poll block requires question");
  }
  if (!block.getArray("options")) {
    return BlockError("poll block requires options array");
  }

  std::ostringstream out;
  out << "<div class=\"chat-poll\"><p class=\"chat-poll-question\">"
      << StructuredTextParser::EscapeText(*block.getString("question"))
      << "</p><div class=\"row chat-poll-options\">";
  for (const Value& option_value : block.getArray("options")->elements) {
    const Object* option = asObject(option_value);
    if (!option || !option->getString("label") || !option->getString("message")) {
      return BlockError("poll options require label and message");
    }
    auto button = AppendChatActionButton(parent, *option->getString("label"), *option->getString("message"),
                                         option->contains("payload") ? ParseOptionalButtonPayload(*option)
                                                                    : std::nullopt);
    if (!button.ok) {
      return button;
    }
    out << button.rml;
  }
  out << "</div></div>";

  ParseResult result;
  result.ok = true;
  result.rml = out.str();
  return result;
}

ParseResult RenderBlock(const Object& block, ParseResult& parent) {
  if (!block.getString("type")) {
    return BlockError("Block must be an object with a type field");
  }

  const std::string type = *block.getString("type");

  if (type == "paragraph") {
    if (!block.getString("text")) {
      return BlockError("paragraph block requires text");
    }
    ParseResult result;
    result.ok = true;
    result.rml = "<p>" + StructuredTextParser::EscapeText(*block.getString("text")) + "</p>";
    return result;
  }

  if (type == "heading") {
    if (!block.getString("text")) {
      return BlockError("heading block requires text");
    }
    int level = 2;
    if (block.contains("level")) {
      auto level_value = block.getIf<int64_t>("level");
      if (!level_value) {
        return BlockError("heading level must be an integer");
      }
      level = static_cast<int>(*level_value);
    }
    if (level < 1 || level > 3) {
      return BlockError("heading level must be 1-3");
    }
    ParseResult result;
    result.ok = true;
    result.rml = "<h" + std::to_string(level) + ">" + StructuredTextParser::EscapeText(*block.getString("text")) +
                 "</h" + std::to_string(level) + ">";
    return result;
  }

  if (type == "list") {
    if (!block.getArray("items")) {
      return BlockError("list block requires items array");
    }
    const bool ordered = block.getIf<bool>("ordered").value_or(false);
    std::ostringstream out;
    out << (ordered ? "<ol>" : "<ul>");
    for (const Value& item_value : block.getArray("items")->elements) {
      auto item_text = asString(item_value);
      if (!item_text) {
        return BlockError("list items must be strings");
      }
      out << "<li>" << StructuredTextParser::EscapeText(*item_text) << "</li>";
    }
    out << (ordered ? "</ol>" : "</ul>");
    ParseResult result;
    result.ok = true;
    result.rml = out.str();
    return result;
  }

  if (type == "code") {
    if (!block.getString("text")) {
      return BlockError("code block requires text");
    }
    ParseResult result;
    result.ok = true;
    result.rml = "<div class=\"code-block\">" + StructuredTextParser::EscapeText(*block.getString("text")) + "</div>";
    return result;
  }

  if (type == "button") {
    return ParseButtonBlock(block, parent);
  }

  if (type == "card") {
    return ParseCardBlock(block);
  }
  if (type == "table") {
    return ParseTableBlock(block);
  }
  if (type == "key_value") {
    return ParseKeyValueBlock(block);
  }
  if (type == "callout") {
    return ParseCalloutBlock(block);
  }
  if (type == "quote") {
    return ParseQuoteBlock(block);
  }
  if (type == "form") {
    return ParseFormBlock(block);
  }
  if (type == "calendar") {
    return ParseCalendarBlock(block);
  }
  if (type == "action_list") {
    return ParseActionListBlock(block, parent);
  }
  if (type == "long_list") {
    return ParseLongListBlock(block, parent);
  }
  if (type == "choice") {
    return ParseChoiceBlock(block, parent);
  }
  if (type == "poll") {
    return ParsePollBlock(block, parent);
  }

  return BlockError("Unknown block type: " + type);
}

std::optional<std::string> ExtractJsonPayload(const std::string& llm_output) {
  static const std::regex json_re(R"re(```json\s*([\s\S]*?)```)re", std::regex::icase);
  std::smatch match;
  if (std::regex_search(llm_output, match, json_re)) {
    return TrimAsciiWhitespace(match[1].str());
  }

  const std::string trimmed = TrimAsciiWhitespace(llm_output);
  if (!trimmed.empty() && trimmed.front() == '{') {
    return trimmed;
  }
  return std::nullopt;
}

bool IsDisplayBlockType(const std::string& type) {
  return type == "paragraph" || type == "heading" || type == "list" || type == "code" || type == "button" ||
         type == "card" || type == "table" || type == "key_value" || type == "callout" || type == "quote" ||
         type == "form" || type == "calendar" || type == "action_list" || type == "long_list" || type == "choice" ||
         type == "poll";
}

bool IsKnownToolName(const std::string& name) {
  return name == "web_search";
}

bool IsEmbeddedToolBlock(const Object& block) {
  if (block.getString("type")) {
    const std::string type = *block.getString("type");
    if (IsDisplayBlockType(type)) {
      return false;
    }
    if (type == "tool" || type == "tool_call" || IsKnownToolName(type)) {
      return true;
    }
  }

  if (block.getString("tool")) {
    return true;
  }

  if (block.getString("name") && IsKnownToolName(*block.getString("name"))) {
    return true;
  }

  return false;
}

Object ToolArgumentsFromBlock(const Object& block) {
  for (const char* key : {"params", "arguments", "parameters"}) {
    if (const Object* args = block.getObject(key)) {
      return *args;
    }
  }
  if (auto query = block.getString("query")) {
    Object args;
    args.set("query", *query);
    return args;
  }
  return {};
}

std::string ToolNameFromBlock(const Object& block) {
  if (auto tool = block.getString("tool")) {
    return *tool;
  }
  if (auto type = block.getString("type")) {
    if (IsKnownToolName(*type)) {
      return *type;
    }
  }
  if (auto name = block.getString("name")) {
    return *name;
  }
  return {};
}

} // namespace

ParseResult StructuredTextParser::ParseBlocksJson(const std::string& json, const ResponseGoal goal,
                                                  const RenderMode render_mode) {
  const std::string trimmed = TrimAsciiWhitespace(json);
  auto doc_opt = TryParseObject(trimmed);
  if (!doc_opt) {
    const std::string repaired = BalanceJsonBraces(trimmed);
    if (repaired != trimmed) {
      doc_opt = TryParseObject(repaired);
    }
  }
  if (!doc_opt) {
    return Fail("Invalid JSON");
  }
  const Object& doc = *doc_opt;
  if (!doc.getArray("blocks")) {
    return Fail("JSON must contain a blocks array");
  }

  std::ostringstream text_stack;
  bool has_text = false;

  ParseResult result;
  result.ok = true;

  int block_index = 0;
  for (const Value& block_value : doc.getArray("blocks")->elements) {
    const Object* block_ptr = asObject(block_value);
    if (!block_ptr) {
      result.warnings.push_back("Block must be an object with a type field");
      ++block_index;
      continue;
    }
    const Object& block = *block_ptr;
    const BlockEligibility eligibility = EvaluateBlock(block, goal);
    const bool undo_inline_long_list_actions =
        eligibility.eligible && eligibility.kind == WorkingSetKind::LongList;
    const size_t chat_actions_before = undo_inline_long_list_actions ? result.chat_actions.size() : 0;

    auto rendered = RenderBlock(block, result);
    if (!rendered.ok) {
      result.warnings.push_back(rendered.error);
      ++block_index;
      continue;
    }

    if (undo_inline_long_list_actions) {
      result.chat_actions.resize(chat_actions_before);
    }

    if (eligibility.eligible) {
      WorkingSetCandidate candidate;
      candidate.block_index = block_index;
      candidate.kind = eligibility.kind;
      candidate.affinity = eligibility.affinity;
      candidate.auto_open = eligibility.auto_open;
      candidate.title = eligibility.title;
      candidate.subtitle = eligibility.subtitle;
      candidate.artifact_rml = BuildArtifactRml(block, eligibility.kind, rendered.rml, result);
      candidate.teaser_rml = BuildWorkingSetTeaser(block_index, eligibility.teaser_label);
      text_stack << candidate.teaser_rml;
      result.working_set_candidates.push_back(std::move(candidate));
    } else {
      text_stack << rendered.rml;
    }

    for (const WidgetInit& init : rendered.widget_inits) {
      result.widget_inits.push_back(init);
    }
    has_text = true;
    ++block_index;
  }

  if (!has_text && !result.warnings.empty()) {
    return Fail("No displayable blocks");
  }

  if (has_text) {
    if (!result.warnings.empty()) {
      text_stack << "<p class=\"muted\">Some blocks could not be displayed.</p>";
    }
    result.rml = "<div class=\"stack\">" + text_stack.str() + "</div>";
  }

  (void)render_mode;
  return result;
}

std::optional<std::vector<EmbeddedToolCall>> StructuredTextParser::ExtractEmbeddedToolCalls(
    const std::string& llm_output) {
  const auto payload = ExtractJsonPayload(llm_output);
  if (!payload) {
    return std::nullopt;
  }

  auto doc_opt = TryParseObject(*payload);
  if (!doc_opt || !doc_opt->getArray("blocks")) {
    return std::nullopt;
  }
  const Object& doc = *doc_opt;

  std::vector<EmbeddedToolCall> tools;
  bool has_display = false;

  for (const Value& block_value : doc.getArray("blocks")->elements) {
    const Object* block_ptr = asObject(block_value);
    if (!block_ptr) {
      continue;
    }
    const Object& block = *block_ptr;
    if (auto type = block.getString("type"); type && IsDisplayBlockType(*type)) {
      has_display = true;
      continue;
    }
    if (!IsEmbeddedToolBlock(block)) {
      continue;
    }

    const std::string name = ToolNameFromBlock(block);
    if (name.empty()) {
      continue;
    }
    tools.push_back({name, ToolArgumentsFromBlock(block)});
  }

  if (tools.empty() || has_display) {
    return std::nullopt;
  }
  return tools;
}

bool StructuredTextParser::IsBlocksJsonDocument(const std::string& text) {
  const std::string trimmed = TrimAsciiWhitespace(text);
  if (trimmed.empty() || trimmed.front() != '{') {
    return false;
  }
  auto doc = TryParseObject(trimmed);
  return doc && doc->getArray("blocks");
}

ParseResult StructuredTextParser::ParseFromLlmOutput(const std::string& llm_output, const ResponseGoal goal,
                                                     const RenderMode render_mode) {
  if (ExtractEmbeddedToolCalls(llm_output)) {
    return Fail("Response contains tool calls, not display blocks");
  }

  static const std::regex json_re(R"re(```json\s*([\s\S]*?)```)re", std::regex::icase);
  std::smatch match;
  if (std::regex_search(llm_output, match, json_re)) {
    return ParseBlocksJson(TrimAsciiWhitespace(match[1].str()), goal, render_mode);
  }

  const std::string trimmed = TrimAsciiWhitespace(llm_output);
  if (!trimmed.empty() && trimmed.front() == '{') {
    auto bare = ParseBlocksJson(trimmed, goal, render_mode);
    if (bare.ok) {
      return bare;
    }
  }

  if (const std::string blocks = TryPeopleDiscoveryBlocksFromToolJson(trimmed); !blocks.empty()) {
    return ParseBlocksJson(blocks, ResponseGoal::PeopleDiscovery, render_mode);
  }

  return Fail("No ```json block found in LLM output");
}

} // namespace pbr
