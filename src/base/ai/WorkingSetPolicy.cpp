#include "base/ai/WorkingSetPolicy.h"

#include "base/ai/StructuredTextParser.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace pbr {

namespace {

std::string Trim(const std::string& text) {
  const auto start = std::find_if_not(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); });
  const auto end = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char c) { return std::isspace(c); }).base();
  if (start >= end) {
    return {};
  }
  return std::string(start, end);
}

int CountLines(const std::string& text) {
  if (text.empty()) {
    return 0;
  }
  int lines = 1;
  for (char c : text) {
    if (c == '\n') {
      ++lines;
    }
  }
  return lines;
}

} // namespace

WorkingSetRouting RouteTurn(const ResponseGoal goal, const RenderMode /*render_mode*/) {
  switch (goal) {
  case ResponseGoal::DisplayFeed:
  case ResponseGoal::PeopleDiscovery:
    return {.panel_primary = true, .auto_open_eligible = true};
  case ResponseGoal::Summarize:
  case ResponseGoal::AnswerQuestion:
  case ResponseGoal::Headlines:
    return {.panel_primary = false, .auto_open_eligible = true};
  case ResponseGoal::General:
  default:
    return {.panel_primary = false, .auto_open_eligible = true};
  }
}

std::string BuildWorkingSetTeaser(const int block_index, const std::string& label) {
  return "<button class=\"chat-working-set-chip\" data-event-click=\"open_working_set('__ENTRY__', " +
         std::to_string(block_index) + ")\">" + StructuredTextParser::EscapeText(label) + "</button>";
}

BlockEligibility EvaluateBlock(const nlohmann::json& block, const ResponseGoal goal) {
  BlockEligibility result;
  if (!block.is_object() || !block.contains("type") || !block["type"].is_string()) {
    return result;
  }

  const std::string type = block["type"].get<std::string>();
  const WorkingSetRouting routing = RouteTurn(goal, RenderMode::Blocks);
  result.promote_to_panel_only = routing.panel_primary;

  if (type == "long_list") {
    if (!block.contains("items") || !block["items"].is_array() || block["items"].empty()) {
      return result;
    }
    result.eligible = true;
    result.kind = WorkingSetKind::LongList;
    result.affinity = WorkingSetAffinity::Feed;
    result.auto_open = routing.auto_open_eligible;
    result.title = block.value("title", "List");
    const size_t count = block["items"].size();
    result.subtitle = std::to_string(count) + (count == 1 ? " item" : " items");
    result.teaser_label = "View in panel (" + std::to_string(count) + " items)";
    return result;
  }

  if (type == "form") {
    result.eligible = true;
    result.kind = WorkingSetKind::Form;
    result.affinity = WorkingSetAffinity::Form;
    result.auto_open = routing.auto_open_eligible;
    result.title = block.value("title", "Form");
    if (block.contains("fields") && block["fields"].is_array()) {
      const size_t count = block["fields"].size();
      result.subtitle = std::to_string(count) + (count == 1 ? " field" : " fields");
      result.teaser_label = "Open form (" + std::to_string(count) + " fields)";
    } else {
      result.subtitle = "";
      result.teaser_label = "Open form";
    }
    return result;
  }

  if (type == "calendar") {
    result.eligible = true;
    result.kind = WorkingSetKind::Calendar;
    result.affinity = WorkingSetAffinity::Form;
    result.auto_open = routing.auto_open_eligible;
    result.title = "Calendar";
    result.subtitle = "";
    result.teaser_label = "Open calendar";
    return result;
  }

  if (type == "table") {
    if (!block.contains("rows") || !block["rows"].is_array()) {
      return result;
    }
    const size_t row_count = block["rows"].size();
    size_t col_count = 0;
    if (block.contains("headers") && block["headers"].is_array()) {
      col_count = block["headers"].size();
    }
    if (row_count <= 4 && col_count <= 3) {
      return result;
    }
    result.eligible = true;
    result.kind = WorkingSetKind::Table;
    result.affinity = WorkingSetAffinity::DataTable;
    result.auto_open = routing.auto_open_eligible;
    result.title = "Table";
    result.subtitle = std::to_string(row_count) + " rows";
    result.teaser_label = "View table (" + std::to_string(row_count) + " rows)";
    return result;
  }

  if (type == "code") {
    if (!block.contains("text") || !block["text"].is_string()) {
      return result;
    }
    const std::string text = block["text"].get<std::string>();
    if (CountLines(text) <= 12 && text.size() <= 400) {
      return result;
    }
    result.eligible = true;
    result.kind = WorkingSetKind::Code;
    result.affinity = WorkingSetAffinity::Document;
    result.auto_open = routing.auto_open_eligible;
    result.title = "Code";
    result.subtitle = std::to_string(CountLines(text)) + " lines";
    result.teaser_label = "View full code";
    return result;
  }

  if (type == "key_value") {
    if (!block.contains("items") || !block["items"].is_array() || block["items"].size() <= 6) {
      return result;
    }
    const size_t count = block["items"].size();
    result.eligible = true;
    result.kind = WorkingSetKind::KeyValue;
    result.affinity = WorkingSetAffinity::Document;
    result.auto_open = routing.auto_open_eligible;
    result.title = "Details";
    result.subtitle = std::to_string(count) + " entries";
    result.teaser_label = "View details (" + std::to_string(count) + " entries)";
    return result;
  }

  if (type == "card") {
    if (!block.contains("body") || !block["body"].is_string()) {
      return result;
    }
    const std::string body = block["body"].get<std::string>();
    if (body.size() <= 200) {
      return result;
    }
    result.eligible = true;
    result.kind = WorkingSetKind::Card;
    result.affinity = WorkingSetAffinity::Document;
    result.auto_open = routing.auto_open_eligible;
    result.title = block.value("title", "Card");
    result.subtitle = "";
    result.teaser_label = "View card";
    return result;
  }

  (void)goal;
  return result;
}

ResponseGoal InferResponseGoalFromBlocksJson(const std::string& json) {
  const nlohmann::json doc = nlohmann::json::parse(Trim(json), nullptr, false);
  if (doc.is_discarded() || !doc.is_object() || !doc.contains("blocks") || !doc["blocks"].is_array()) {
    return ResponseGoal::General;
  }

  bool has_long_list = false;
  for (const auto& block : doc["blocks"]) {
    if (!block.is_object() || !block.contains("type") || !block["type"].is_string()) {
      continue;
    }
    const std::string type = block["type"].get<std::string>();
    if (type == "long_list") {
      has_long_list = true;
    }
  }

  if (has_long_list) {
    return ResponseGoal::PeopleDiscovery;
  }
  return ResponseGoal::General;
}

} // namespace pbr
