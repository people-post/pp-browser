#include "base/ai/StructuredTextParser.h"

#include "base/ai/WorkingSetPolicy.h"
#include "base/messaging/PeopleDiscoveryBlocks.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>

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

std::optional<std::string> ParseOptionalButtonPayload(const nlohmann::json& block) {
  if (!block.contains("payload")) {
    return std::nullopt;
  }
  const nlohmann::json& payload = block["payload"];
  if (payload.is_object()) {
    return payload.dump();
  }
  if (!payload.is_string()) {
    return std::nullopt;
  }
  const std::string payload_str = payload.get<std::string>();
  if (payload_str.empty()) {
    return std::nullopt;
  }
  const nlohmann::json doc = nlohmann::json::parse(payload_str, nullptr, false);
  if (doc.is_discarded() || !doc.is_object()) {
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

ParseResult ParseButtonBlock(const nlohmann::json& block, ParseResult& parent) {
  if (!block.contains("label") || !block["label"].is_string()) {
    return BlockError("button block requires label");
  }
  if (!block.contains("message") || !block["message"].is_string()) {
    return BlockError("button block requires message");
  }

  std::optional<std::string> payload;
  if (block.contains("payload")) {
    payload = ParseOptionalButtonPayload(block);
    if (!payload) {
      return BlockError("button payload must be a JSON object or object string");
    }
  }

  return AppendChatActionButton(parent, block["label"].get<std::string>(), block["message"].get<std::string>(),
                                payload);
}

ParseResult ParseCardBlock(const nlohmann::json& block) {
  if (!block.contains("title") || !block["title"].is_string()) {
    return BlockError("card block requires title");
  }
  if (!block.contains("body") || !block["body"].is_string()) {
    return BlockError("card block requires body");
  }

  const std::string variant = block.value("variant", "default");
  std::ostringstream out;
  out << "<div class=\"chat-card chat-card-" << StructuredTextParser::EscapeText(variant) << "\">";
  out << "<h3>" << StructuredTextParser::EscapeText(block["title"].get<std::string>()) << "</h3>";
  if (block.contains("subtitle") && block["subtitle"].is_string()) {
    out << "<p class=\"muted\">" << StructuredTextParser::EscapeText(block["subtitle"].get<std::string>()) << "</p>";
  }
  out << "<p>" << StructuredTextParser::EscapeText(block["body"].get<std::string>()) << "</p>";
  out << "</div>";

  ParseResult result;
  result.ok = true;
  result.rml = out.str();
  return result;
}

ParseResult ParseTableBlock(const nlohmann::json& block) {
  if (!block.contains("headers") || !block["headers"].is_array()) {
    return BlockError("table block requires headers array");
  }
  if (!block.contains("rows") || !block["rows"].is_array()) {
    return BlockError("table block requires rows array");
  }

  std::ostringstream out;
  out << "<table class=\"chat-table\"><thead><tr>";
  for (const auto& header : block["headers"]) {
    if (!header.is_string()) {
      return BlockError("table headers must be strings");
    }
    out << "<th>" << StructuredTextParser::EscapeText(header.get<std::string>()) << "</th>";
  }
  out << "</tr></thead><tbody>";
  for (const auto& row : block["rows"]) {
    if (!row.is_array()) {
      return BlockError("table rows must be arrays");
    }
    out << "<tr>";
    for (const auto& cell : row) {
      if (!cell.is_string()) {
        return BlockError("table cells must be strings");
      }
      out << "<td>" << StructuredTextParser::EscapeText(cell.get<std::string>()) << "</td>";
    }
    out << "</tr>";
  }
  out << "</tbody></table>";

  ParseResult result;
  result.ok = true;
  result.rml = out.str();
  return result;
}

ParseResult ParseKeyValueBlock(const nlohmann::json& block) {
  if (!block.contains("items") || !block["items"].is_array()) {
    return BlockError("key_value block requires items array");
  }

  std::ostringstream out;
  out << "<div class=\"chat-key-value\">";
  for (const auto& item : block["items"]) {
    if (!item.is_object() || !item.contains("label") || !item.contains("value")) {
      return BlockError("key_value items require label and value");
    }
    if (!item["label"].is_string() || !item["value"].is_string()) {
      return BlockError("key_value label and value must be strings");
    }
    out << "<div class=\"chat-key-value-row\">";
    out << "<span class=\"chat-key-value-label\">" << StructuredTextParser::EscapeText(item["label"].get<std::string>())
        << "</span>";
    out << "<span class=\"chat-key-value-value\">" << StructuredTextParser::EscapeText(item["value"].get<std::string>())
        << "</span>";
    out << "</div>";
  }
  out << "</div>";

  ParseResult result;
  result.ok = true;
  result.rml = out.str();
  return result;
}

ParseResult ParseCalloutBlock(const nlohmann::json& block) {
  if (!block.contains("text") || !block["text"].is_string()) {
    return BlockError("callout block requires text");
  }
  const std::string variant = block.value("variant", "info");
  ParseResult result;
  result.ok = true;
  result.rml = "<div class=\"chat-callout chat-callout-" + StructuredTextParser::EscapeText(variant) + "\"><p>" +
               StructuredTextParser::EscapeText(block["text"].get<std::string>()) + "</p></div>";
  return result;
}

ParseResult ParseQuoteBlock(const nlohmann::json& block) {
  if (!block.contains("text") || !block["text"].is_string()) {
    return BlockError("quote block requires text");
  }
  std::ostringstream out;
  out << "<blockquote class=\"chat-quote\"><p>" << StructuredTextParser::EscapeText(block["text"].get<std::string>())
      << "</p>";
  if (block.contains("attribution") && block["attribution"].is_string()) {
    out << "<p class=\"muted\">— " << StructuredTextParser::EscapeText(block["attribution"].get<std::string>()) << "</p>";
  }
  out << "</blockquote>";

  ParseResult result;
  result.ok = true;
  result.rml = out.str();
  return result;
}

ParseResult ParseFormBlock(const nlohmann::json& block) {
  if (!block.contains("id") || !block["id"].is_string()) {
    return BlockError("form block requires id");
  }
  if (!block.contains("fields") || !block["fields"].is_array()) {
    return BlockError("form block requires fields array");
  }
  if (!block.contains("submit_template") || !block["submit_template"].is_string()) {
    return BlockError("form block requires submit_template");
  }

  for (const auto& field : block["fields"]) {
    if (!field.is_object() || !field.contains("id") || !field["id"].is_string()) {
      return BlockError("form fields require id");
    }
    if (!field.contains("label") || !field["label"].is_string()) {
      return BlockError("form fields require label");
    }
  }

  const std::string form_id = block["id"].get<std::string>();
  ParseResult result;
  result.ok = true;
  result.rml = ReplaceAll(ReplaceAll(kFormWidgetRml, "__FORM_ID__", form_id), "__ENTRY__", "__ENTRY__");
  result.widget_inits.push_back({WidgetInitKind::Form, block});
  return result;
}

ParseResult ParseCalendarBlock(const nlohmann::json& block) {
  if (block.contains("month")) {
    if (!block["month"].is_number_integer()) {
      return BlockError("calendar month must be an integer");
    }
  }
  if (block.contains("year")) {
    if (!block["year"].is_number_integer()) {
      return BlockError("calendar year must be an integer");
    }
  }

  ParseResult result;
  result.ok = true;
  result.rml = kCalendarWidgetRml;
  result.widget_inits.push_back({WidgetInitKind::Calendar, block});
  return result;
}

ParseResult ParseLongListActionButton(ParseResult& parent, const nlohmann::json& action) {
  if (!action.is_object() || !action.contains("label") || !action.contains("message")) {
    return BlockError("long_list actions require label and message");
  }
  if (!action["label"].is_string() || !action["message"].is_string()) {
    return BlockError("long_list action label and message must be strings");
  }
  if (action.contains("payload")) {
    const auto payload = ParseOptionalButtonPayload(action);
    if (!payload) {
      return BlockError("long_list action payload must be a JSON object or object string");
    }
    return AppendChatActionButton(parent, action["label"].get<std::string>(), action["message"].get<std::string>(),
                                  payload);
  }
  return AppendChatActionButton(parent, action["label"].get<std::string>(), action["message"].get<std::string>(),
                                std::nullopt);
}

ParseResult ParseLongListBlock(const nlohmann::json& block, ParseResult& parent) {
  if (!block.contains("items") || !block["items"].is_array()) {
    return BlockError("long_list block requires items array");
  }

  std::ostringstream out;
  out << "<div class=\"chat-long-list\">";
  if (block.contains("title") && block["title"].is_string()) {
    out << "<p class=\"muted\">" << StructuredTextParser::EscapeText(block["title"].get<std::string>()) << "</p>";
  }
  out << "<div class=\"chat-long-list-scroll\">";
  for (const auto& item : block["items"]) {
    if (!item.is_object() || !item.contains("title") || !item["title"].is_string()) {
      return BlockError("long_list items require title");
    }
    out << "<div class=\"chat-long-list-item\">";
    out << "<p class=\"chat-long-list-title\">" << StructuredTextParser::EscapeText(item["title"].get<std::string>())
        << "</p>";
    if (item.contains("subtitle") && item["subtitle"].is_string()) {
      out << "<p class=\"muted chat-long-list-subtitle\">"
          << StructuredTextParser::EscapeText(item["subtitle"].get<std::string>()) << "</p>";
    }
    if (item.contains("meta") && item["meta"].is_string()) {
      out << "<p class=\"muted chat-long-list-meta\">" << StructuredTextParser::EscapeText(item["meta"].get<std::string>())
          << "</p>";
    }
    if (item.contains("actions") && item["actions"].is_array()) {
      out << "<div class=\"row chat-long-list-actions\">";
      for (const auto& action : item["actions"]) {
        auto button = ParseLongListActionButton(parent, action);
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
  if (block.contains("footer_actions") && block["footer_actions"].is_array()) {
    out << "<div class=\"row chat-long-list-footer\">";
    for (const auto& action : block["footer_actions"]) {
      auto button = ParseLongListActionButton(parent, action);
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

ParseResult ParseLongListArtifact(const nlohmann::json& block, ParseResult& parent) {
  if (!block.contains("items") || !block["items"].is_array()) {
    return BlockError("long_list block requires items array");
  }

  std::ostringstream out;
  out << "<div class=\"working-set-long-list\">";
  if (block.contains("title") && block["title"].is_string()) {
    out << "<p class=\"muted\">" << StructuredTextParser::EscapeText(block["title"].get<std::string>()) << "</p>";
  }
  out << "<div class=\"working-set-long-list-body\">";
  for (const auto& item : block["items"]) {
    if (!item.is_object() || !item.contains("title") || !item["title"].is_string()) {
      return BlockError("long_list items require title");
    }
    out << "<div class=\"chat-long-list-item\">";
    out << "<p class=\"chat-long-list-title\">" << StructuredTextParser::EscapeText(item["title"].get<std::string>())
        << "</p>";
    if (item.contains("subtitle") && item["subtitle"].is_string()) {
      out << "<p class=\"muted chat-long-list-subtitle\">"
          << StructuredTextParser::EscapeText(item["subtitle"].get<std::string>()) << "</p>";
    }
    if (item.contains("meta") && item["meta"].is_string()) {
      out << "<p class=\"muted chat-long-list-meta\">" << StructuredTextParser::EscapeText(item["meta"].get<std::string>())
          << "</p>";
    }
    if (item.contains("actions") && item["actions"].is_array()) {
      out << "<div class=\"row chat-long-list-actions\">";
      for (const auto& action : item["actions"]) {
        auto button = ParseLongListActionButton(parent, action);
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
  if (block.contains("footer_actions") && block["footer_actions"].is_array()) {
    out << "<div class=\"row chat-long-list-footer\">";
    for (const auto& action : block["footer_actions"]) {
      auto button = ParseLongListActionButton(parent, action);
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

std::string BuildArtifactRml(const nlohmann::json& block, const WorkingSetKind kind, const std::string& inline_rml,
                             ParseResult& parent) {
  switch (kind) {
  case WorkingSetKind::LongList: {
    const auto artifact = ParseLongListArtifact(block, parent);
    return artifact.ok ? artifact.rml : inline_rml;
  }
  case WorkingSetKind::Form: {
    const std::string form_id = block["id"].get<std::string>();
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

ParseResult ParseActionListBlock(const nlohmann::json& block, ParseResult& parent) {
  if (!block.contains("items") || !block["items"].is_array()) {
    return BlockError("action_list block requires items array");
  }

  std::ostringstream out;
  out << "<div class=\"chat-action-list\">";
  for (const auto& item : block["items"]) {
    if (!item.is_object() || !item.contains("title") || !item["title"].is_string()) {
      return BlockError("action_list items require title");
    }
    out << "<div class=\"chat-action-list-item\">";
    out << "<p class=\"chat-action-list-title\">" << StructuredTextParser::EscapeText(item["title"].get<std::string>())
        << "</p>";
    if (item.contains("description") && item["description"].is_string()) {
      out << "<p class=\"muted\">" << StructuredTextParser::EscapeText(item["description"].get<std::string>()) << "</p>";
    }
    if (item.contains("actions") && item["actions"].is_array()) {
      out << "<div class=\"row chat-action-list-actions\">";
      for (const auto& action : item["actions"]) {
        if (!action.is_object() || !action.contains("label") || !action.contains("message")) {
          return BlockError("action_list actions require label and message");
        }
        auto button = AppendChatActionButton(parent, action["label"].get<std::string>(),
                                               action["message"].get<std::string>(),
                                               action.contains("payload") ? ParseOptionalButtonPayload(action)
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

ParseResult ParseChoiceBlock(const nlohmann::json& block, ParseResult& parent) {
  if (!block.contains("prompt") || !block["prompt"].is_string()) {
    return BlockError("choice block requires prompt");
  }
  if (!block.contains("options") || !block["options"].is_array()) {
    return BlockError("choice block requires options array");
  }

  std::ostringstream out;
  out << "<div class=\"chat-choice\"><p>" << StructuredTextParser::EscapeText(block["prompt"].get<std::string>())
      << "</p><div class=\"row chat-choice-options\">";
  for (const auto& option : block["options"]) {
    if (!option.is_object() || !option.contains("label") || !option.contains("message")) {
      return BlockError("choice options require label and message");
    }
    auto button = AppendChatActionButton(parent, option["label"].get<std::string>(), option["message"].get<std::string>(),
                                         option.contains("payload") ? ParseOptionalButtonPayload(option)
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

ParseResult ParsePollBlock(const nlohmann::json& block, ParseResult& parent) {
  if (!block.contains("question") || !block["question"].is_string()) {
    return BlockError("poll block requires question");
  }
  if (!block.contains("options") || !block["options"].is_array()) {
    return BlockError("poll block requires options array");
  }

  std::ostringstream out;
  out << "<div class=\"chat-poll\"><p class=\"chat-poll-question\">"
      << StructuredTextParser::EscapeText(block["question"].get<std::string>())
      << "</p><div class=\"row chat-poll-options\">";
  for (const auto& option : block["options"]) {
    if (!option.is_object() || !option.contains("label") || !option.contains("message")) {
      return BlockError("poll options require label and message");
    }
    auto button = AppendChatActionButton(parent, option["label"].get<std::string>(), option["message"].get<std::string>(),
                                         option.contains("payload") ? ParseOptionalButtonPayload(option)
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

ParseResult RenderBlock(const nlohmann::json& block, ParseResult& parent) {
  if (!block.is_object() || !block.contains("type") || !block["type"].is_string()) {
    return BlockError("Block must be an object with a type field");
  }

  const std::string type = block["type"].get<std::string>();

  if (type == "paragraph") {
    if (!block.contains("text") || !block["text"].is_string()) {
      return BlockError("paragraph block requires text");
    }
    ParseResult result;
    result.ok = true;
    result.rml = "<p>" + StructuredTextParser::EscapeText(block["text"].get<std::string>()) + "</p>";
    return result;
  }

  if (type == "heading") {
    if (!block.contains("text") || !block["text"].is_string()) {
      return BlockError("heading block requires text");
    }
    int level = 2;
    if (block.contains("level")) {
      if (!block["level"].is_number_integer()) {
        return BlockError("heading level must be an integer");
      }
      level = block["level"].get<int>();
    }
    if (level < 1 || level > 3) {
      return BlockError("heading level must be 1-3");
    }
    ParseResult result;
    result.ok = true;
    result.rml = "<h" + std::to_string(level) + ">" + StructuredTextParser::EscapeText(block["text"].get<std::string>()) +
                 "</h" + std::to_string(level) + ">";
    return result;
  }

  if (type == "list") {
    if (!block.contains("items") || !block["items"].is_array()) {
      return BlockError("list block requires items array");
    }
    const bool ordered = block.value("ordered", false);
    std::ostringstream out;
    out << (ordered ? "<ol>" : "<ul>");
    for (const auto& item : block["items"]) {
      if (!item.is_string()) {
        return BlockError("list items must be strings");
      }
      out << "<li>" << StructuredTextParser::EscapeText(item.get<std::string>()) << "</li>";
    }
    out << (ordered ? "</ol>" : "</ul>");
    ParseResult result;
    result.ok = true;
    result.rml = out.str();
    return result;
  }

  if (type == "code") {
    if (!block.contains("text") || !block["text"].is_string()) {
      return BlockError("code block requires text");
    }
    ParseResult result;
    result.ok = true;
    result.rml = "<div class=\"code-block\">" + StructuredTextParser::EscapeText(block["text"].get<std::string>()) + "</div>";
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

bool IsEmbeddedToolBlock(const nlohmann::json& block) {
  if (!block.is_object()) {
    return false;
  }

  if (block.contains("type") && block["type"].is_string()) {
    const std::string type = block["type"].get<std::string>();
    if (IsDisplayBlockType(type)) {
      return false;
    }
    if (type == "tool" || type == "tool_call" || IsKnownToolName(type)) {
      return true;
    }
  }

  if (block.contains("tool") && block["tool"].is_string()) {
    return true;
  }

  if (block.contains("name") && block["name"].is_string() && IsKnownToolName(block["name"].get<std::string>())) {
    return true;
  }

  return false;
}

nlohmann::json ToolArgumentsFromBlock(const nlohmann::json& block) {
  for (const char* key : {"params", "arguments", "parameters"}) {
    if (block.contains(key) && block[key].is_object()) {
      return block[key];
    }
  }
  if (block.contains("query") && block["query"].is_string()) {
    return {{"query", block["query"]}};
  }
  return nlohmann::json::object();
}

std::string ToolNameFromBlock(const nlohmann::json& block) {
  if (block.contains("tool") && block["tool"].is_string()) {
    return block["tool"].get<std::string>();
  }
  if (block.contains("type") && block["type"].is_string()) {
    const std::string type = block["type"].get<std::string>();
    if (IsKnownToolName(type)) {
      return type;
    }
  }
  if (block.contains("name") && block["name"].is_string()) {
    return block["name"].get<std::string>();
  }
  return {};
}

} // namespace

ParseResult StructuredTextParser::ParseBlocksJson(const std::string& json, const ResponseGoal goal,
                                                  const RenderMode render_mode) {
  const std::string trimmed = TrimAsciiWhitespace(json);
  nlohmann::json doc = nlohmann::json::parse(trimmed, nullptr, false);
  if (doc.is_discarded()) {
    const std::string repaired = BalanceJsonBraces(trimmed);
    if (repaired != trimmed)
      doc = nlohmann::json::parse(repaired, nullptr, false);
  }
  if (doc.is_discarded()) {
    return Fail("Invalid JSON");
  }

  if (!doc.is_object() || !doc.contains("blocks") || !doc["blocks"].is_array()) {
    return Fail("JSON must contain a blocks array");
  }

  std::ostringstream text_stack;
  bool has_text = false;

  ParseResult result;
  result.ok = true;

  int block_index = 0;
  for (const auto& block : doc["blocks"]) {
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

  nlohmann::json doc = nlohmann::json::parse(*payload, nullptr, false);
  if (doc.is_discarded() || !doc.is_object() || !doc.contains("blocks") || !doc["blocks"].is_array()) {
    return std::nullopt;
  }

  std::vector<EmbeddedToolCall> tools;
  bool has_display = false;

  for (const auto& block : doc["blocks"]) {
    if (!block.is_object()) {
      continue;
    }
    if (block.contains("type") && block["type"].is_string() &&
        IsDisplayBlockType(block["type"].get<std::string>())) {
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
  const nlohmann::json doc = nlohmann::json::parse(trimmed, nullptr, false);
  return !doc.is_discarded() && doc.is_object() && doc.contains("blocks") && doc["blocks"].is_array();
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
