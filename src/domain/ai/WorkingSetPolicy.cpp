#include "domain/ai/WorkingSetPolicy.h"

#include "domain/ai/StructuredTextParser.h"
#include "common/Utilities.h"
#include "common/ValueJson.h"

#include <sstream>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

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

BlockEligibility EvaluateBlock(const Object& block, const ResponseGoal goal) {
  BlockEligibility result;
  const auto type = block.getString("type");
  if (!type) {
    return result;
  }

  const WorkingSetRouting routing = RouteTurn(goal, RenderMode::Blocks);
  result.promote_to_panel_only = routing.panel_primary;

  if (*type == "long_list") {
    const Array* items = block.getArray("items");
    if (!items || items->elements.empty()) {
      return result;
    }
    result.eligible = true;
    result.kind = WorkingSetKind::LongList;
    result.affinity = WorkingSetAffinity::Feed;
    result.auto_open = routing.auto_open_eligible;
    result.title = block.getString("title").value_or("List");
    const size_t count = items->elements.size();
    result.subtitle = std::to_string(count) + (count == 1 ? " item" : " items");
    result.teaser_label = "View in panel (" + std::to_string(count) + " items)";
    return result;
  }

  if (*type == "form") {
    result.eligible = true;
    result.kind = WorkingSetKind::Form;
    result.affinity = WorkingSetAffinity::Form;
    result.auto_open = routing.auto_open_eligible;
    result.title = block.getString("title").value_or("Form");
    if (const Array* fields = block.getArray("fields")) {
      const size_t count = fields->elements.size();
      result.subtitle = std::to_string(count) + (count == 1 ? " field" : " fields");
      result.teaser_label = "Open form (" + std::to_string(count) + " fields)";
    } else {
      result.subtitle = "";
      result.teaser_label = "Open form";
    }
    return result;
  }

  if (*type == "calendar") {
    result.eligible = true;
    result.kind = WorkingSetKind::Calendar;
    result.affinity = WorkingSetAffinity::Form;
    result.auto_open = routing.auto_open_eligible;
    result.title = "Calendar";
    result.subtitle = "";
    result.teaser_label = "Open calendar";
    return result;
  }

  if (*type == "table") {
    const Array* rows = block.getArray("rows");
    if (!rows) {
      return result;
    }
    const size_t row_count = rows->elements.size();
    size_t col_count = 0;
    if (const Array* headers = block.getArray("headers")) {
      col_count = headers->elements.size();
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

  if (*type == "code") {
    const auto text = block.getString("text");
    if (!text) {
      return result;
    }
    if (CountLines(*text) <= 12 && text->size() <= 400) {
      return result;
    }
    result.eligible = true;
    result.kind = WorkingSetKind::Code;
    result.affinity = WorkingSetAffinity::Document;
    result.auto_open = routing.auto_open_eligible;
    result.title = "Code";
    result.subtitle = std::to_string(CountLines(*text)) + " lines";
    result.teaser_label = "View full code";
    return result;
  }

  if (*type == "key_value") {
    const Array* items = block.getArray("items");
    if (!items || items->elements.size() <= 6) {
      return result;
    }
    const size_t count = items->elements.size();
    result.eligible = true;
    result.kind = WorkingSetKind::KeyValue;
    result.affinity = WorkingSetAffinity::Document;
    result.auto_open = routing.auto_open_eligible;
    result.title = "Details";
    result.subtitle = std::to_string(count) + " entries";
    result.teaser_label = "View details (" + std::to_string(count) + " entries)";
    return result;
  }

  if (*type == "card") {
    const auto body = block.getString("body");
    if (!body) {
      return result;
    }
    if (body->size() <= 200) {
      return result;
    }
    result.eligible = true;
    result.kind = WorkingSetKind::Card;
    result.affinity = WorkingSetAffinity::Document;
    result.auto_open = routing.auto_open_eligible;
    result.title = block.getString("title").value_or("Card");
    result.subtitle = "";
    result.teaser_label = "View card";
    return result;
  }

  (void)goal;
  return result;
}

ResponseGoal InferResponseGoalFromBlocksJson(const std::string& json) {
  auto doc = TryParseObject(util::Trim(json));
  if (!doc) {
    return ResponseGoal::General;
  }
  const Array* blocks = doc->getArray("blocks");
  if (!blocks) {
    return ResponseGoal::General;
  }

  bool has_long_list = false;
  for (const Value& block_value : blocks->elements) {
    const Object* block = asObject(block_value);
    if (!block) {
      continue;
    }
    if (block->getString("type").value_or("") == "long_list") {
      has_long_list = true;
    }
  }

  if (has_long_list) {
    return ResponseGoal::PeopleDiscovery;
  }
  return ResponseGoal::General;
}

} // namespace pbr
