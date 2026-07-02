#include "base/messaging/ConversationSummaryCodec.h"

#include "base/messaging/MessagingLimits.h"

#include <nlohmann/json.hpp>

namespace pbr {

Roe<std::string> ConversationSummaryCodec::Encode(const ConversationSummary& summary) {
  if (summary.text.size() > kMaxSummaryBytes) {
    return Error("Summary text exceeds kMaxSummaryBytes");
  }
  nlohmann::json json;
  json["schema_version"] = summary.schema_version;
  json["version"] = summary.version;
  json["text"] = summary.text;
  if (summary.compacted_through_display_order.has_value()) {
    json["compacted_through_display_order"] = *summary.compacted_through_display_order;
  }
  json["updated_at"] = summary.updated_at;
  return json.dump();
}

Roe<ConversationSummary> ConversationSummaryCodec::Decode(const std::string& json_text) {
  nlohmann::json json;
  try {
    json = nlohmann::json::parse(json_text);
  } catch (const nlohmann::json::exception&) {
    return Error("Invalid ConversationSummary JSON");
  }

  ConversationSummary summary;
  if (!json.contains("schema_version") || !json["schema_version"].is_number_integer()) {
    return Error("ConversationSummary missing schema_version");
  }
  summary.schema_version = json["schema_version"].get<int>();
  if (summary.schema_version != kSchemaVersion) {
    return Error("Unsupported ConversationSummary schema_version");
  }
  if (!json.contains("version") || !json["version"].is_number_integer()) {
    return Error("ConversationSummary missing version");
  }
  summary.version = json["version"].get<int>();
  if (!json.contains("text") || !json["text"].is_string()) {
    return Error("ConversationSummary missing text");
  }
  summary.text = json["text"].get<std::string>();
  if (summary.text.size() > kMaxSummaryBytes) {
    return Error("Summary text exceeds kMaxSummaryBytes");
  }
  if (json.contains("compacted_through_display_order") && json["compacted_through_display_order"].is_number_integer()) {
    summary.compacted_through_display_order = json["compacted_through_display_order"].get<int64_t>();
  }
  if (!json.contains("updated_at") || !json["updated_at"].is_number_integer()) {
    return Error("ConversationSummary missing updated_at");
  }
  summary.updated_at = json["updated_at"].get<int64_t>();
  return summary;
}

} // namespace pbr
