#include "common/ui/WorkingSetCodec.h"

#include "common/ValueJson.h"
#include "common/PbrCompat.h"

namespace pbr {
namespace {

const char* KindName(const WorkingSetKind kind) {
  switch (kind) {
  case WorkingSetKind::LongList:
    return "long_list";
  case WorkingSetKind::Form:
    return "form";
  case WorkingSetKind::Calendar:
    return "calendar";
  case WorkingSetKind::Table:
    return "table";
  case WorkingSetKind::Code:
    return "code";
  case WorkingSetKind::KeyValue:
    return "key_value";
  case WorkingSetKind::Card:
    return "card";
  case WorkingSetKind::None:
    return "none";
  }
  return "none";
}

WorkingSetKind KindFromName(const std::string& name) {
  if (name == "long_list") {
    return WorkingSetKind::LongList;
  }
  if (name == "form") {
    return WorkingSetKind::Form;
  }
  if (name == "calendar") {
    return WorkingSetKind::Calendar;
  }
  if (name == "table") {
    return WorkingSetKind::Table;
  }
  if (name == "code") {
    return WorkingSetKind::Code;
  }
  if (name == "key_value") {
    return WorkingSetKind::KeyValue;
  }
  if (name == "card") {
    return WorkingSetKind::Card;
  }
  return WorkingSetKind::None;
}

const char* AffinityName(const WorkingSetAffinity affinity) {
  switch (affinity) {
  case WorkingSetAffinity::Feed:
    return "feed";
  case WorkingSetAffinity::Form:
    return "form";
  case WorkingSetAffinity::DataTable:
    return "data_table";
  case WorkingSetAffinity::Document:
    return "document";
  case WorkingSetAffinity::None:
    return "none";
  }
  return "none";
}

WorkingSetAffinity AffinityFromName(const std::string& name) {
  if (name == "feed") {
    return WorkingSetAffinity::Feed;
  }
  if (name == "form") {
    return WorkingSetAffinity::Form;
  }
  if (name == "data_table") {
    return WorkingSetAffinity::DataTable;
  }
  if (name == "document") {
    return WorkingSetAffinity::Document;
  }
  return WorkingSetAffinity::None;
}

}  // namespace

std::string WorkingSetCandidatesToJson(const std::vector<WorkingSetCandidate>& candidates) {
  std::vector<Value> rows;
  rows.reserve(candidates.size());
  for (const WorkingSetCandidate& candidate : candidates) {
    Object row;
    row.set("block_index", static_cast<int64_t>(candidate.block_index));
    row.set("kind", KindName(candidate.kind));
    row.set("affinity", AffinityName(candidate.affinity));
    row.set("auto_open", candidate.auto_open);
    row.set("title", candidate.title);
    row.set("subtitle", candidate.subtitle);
    row.set("artifact_rml", candidate.artifact_rml);
    row.set("teaser_rml", candidate.teaser_rml);
    rows.push_back(ObjectValue(std::move(row)));
  }
  Object root;
  root.set("candidates", ArrayValue(std::move(rows)));
  return DumpJson(root);
}

std::vector<WorkingSetCandidate> WorkingSetCandidatesFromJson(const std::string& json) {
  std::vector<WorkingSetCandidate> out;
  if (json.empty()) {
    return out;
  }
  auto parsed = TryParseObject(json);
  if (!parsed) {
    return out;
  }
  const Array* candidates = parsed->getArray("candidates");
  if (!candidates) {
    return out;
  }
  for (const Value& item_value : candidates->elements) {
    const Object* item = asObject(item_value);
    if (!item) {
      continue;
    }
    WorkingSetCandidate candidate;
    if (auto block_index = item->getIf<int64_t>("block_index")) {
      candidate.block_index = static_cast<int>(*block_index);
    }
    candidate.kind = KindFromName(item->getString("kind").value_or("none"));
    candidate.affinity = AffinityFromName(item->getString("affinity").value_or("none"));
    candidate.auto_open = item->getIf<bool>("auto_open").value_or(false);
    candidate.title = item->getString("title").value_or("");
    candidate.subtitle = item->getString("subtitle").value_or("");
    candidate.artifact_rml = item->getString("artifact_rml").value_or("");
    candidate.teaser_rml = item->getString("teaser_rml").value_or("");
    if (candidate.artifact_rml.empty()) {
      continue;
    }
    out.push_back(std::move(candidate));
  }
  return out;
}

bool ContentRmlHasWorkingSetChip(const std::string& rml) {
  return rml.find("chat-working-set-chip") != std::string::npos;
}

bool ContentRmlHasActiveWorkingSetChip(const std::string& rml) {
  return ContentRmlHasWorkingSetChip(rml) && rml.find("open_working_set") != std::string::npos;
}

std::string MarkWorkingSetChipsUnavailable(const std::string& rml) {
  if (!ContentRmlHasActiveWorkingSetChip(rml)) {
    return rml;
  }
  // Replace active reopen chips with a muted, non-interactive marker.
  // Keep surrounding bubble markup intact.
  std::string out;
  out.reserve(rml.size());
  size_t pos = 0;
  while (pos < rml.size()) {
    const size_t chip = rml.find("chat-working-set-chip", pos);
    if (chip == std::string::npos) {
      out.append(rml, pos, std::string::npos);
      break;
    }
    const size_t button_start = rml.rfind("<button", chip);
    const size_t button_end = rml.find("</button>", chip);
    if (button_start == std::string::npos || button_end == std::string::npos || button_start < pos ||
        button_start > chip) {
      out.append(rml, pos, chip - pos + 1);
      pos = chip + 1;
      continue;
    }
    out.append(rml, pos, button_start - pos);
    out += "<button class=\"chat-working-set-chip chat-working-set-chip-disabled\" type=\"button\" "
           "disabled>Results no longer available</button>";
    pos = button_end + 9;  // len("</button>")
  }
  return out;
}

std::string StripInlinedWorkingSetActionSuggestions(const std::string& rml) {
  if (rml.find("chat-suggestion") == std::string::npos ||
      rml.find("send_chat_action") == std::string::npos) {
    return rml;
  }
  std::string out;
  out.reserve(rml.size());
  size_t pos = 0;
  while (pos < rml.size()) {
    const size_t button_start = rml.find("<button", pos);
    if (button_start == std::string::npos) {
      out.append(rml, pos, std::string::npos);
      break;
    }
    const size_t button_end = rml.find("</button>", button_start);
    if (button_end == std::string::npos) {
      out.append(rml, pos, std::string::npos);
      break;
    }
    const std::string button = rml.substr(button_start, button_end + 9 - button_start);
    const bool is_panel_action_dump =
        button.find("chat-suggestion") != std::string::npos &&
        button.find("send_chat_action") != std::string::npos &&
        button.find("chat-working-set-chip") == std::string::npos;
    out.append(rml, pos, button_start - pos);
    if (!is_panel_action_dump) {
      out += button;
    }
    pos = button_end + 9;
  }
  return out;
}

}  // namespace pbr
