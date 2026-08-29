#include "base/messaging/ConversationSummaryCodec.h"

#include "base/messaging/MessagingLimits.h"
#include "common/ValueJson.h"
#include "common/PbrCompat.h"

namespace pbr {

Roe<std::string> ConversationSummaryCodec::Encode(const ConversationSummary& summary) {
  if (summary.text.size() > kMaxSummaryBytes) {
    return Error("Summary text exceeds kMaxSummaryBytes");
  }
  Object json;
  json.set("schema_version", static_cast<int64_t>(summary.schema_version));
  json.set("version", static_cast<int64_t>(summary.version));
  json.set("text", summary.text);
  if (summary.compacted_through_display_order.has_value()) {
    json.set("compacted_through_display_order", *summary.compacted_through_display_order);
  }
  json.set("updated_at", summary.updated_at);
  return DumpJson(json);
}

Roe<ConversationSummary> ConversationSummaryCodec::Decode(const std::string& json_text) {
  auto json = TryParseObject(json_text);
  if (!json) {
    return Error("Invalid ConversationSummary JSON");
  }

  ConversationSummary summary;
  auto schema_version = json->getIf<int64_t>("schema_version");
  if (!schema_version) {
    return Error("ConversationSummary missing schema_version");
  }
  summary.schema_version = static_cast<int>(*schema_version);
  if (summary.schema_version != kSchemaVersion) {
    return Error("Unsupported ConversationSummary schema_version");
  }
  auto version = json->getIf<int64_t>("version");
  if (!version) {
    return Error("ConversationSummary missing version");
  }
  summary.version = static_cast<int>(*version);
  auto text = json->getString("text");
  if (!text) {
    return Error("ConversationSummary missing text");
  }
  summary.text = *text;
  if (summary.text.size() > kMaxSummaryBytes) {
    return Error("Summary text exceeds kMaxSummaryBytes");
  }
  if (auto compacted = json->getIf<int64_t>("compacted_through_display_order")) {
    summary.compacted_through_display_order = *compacted;
  }
  auto updated_at = json->getIf<int64_t>("updated_at");
  if (!updated_at) {
    return Error("ConversationSummary missing updated_at");
  }
  summary.updated_at = *updated_at;
  return summary;
}

} // namespace pbr
