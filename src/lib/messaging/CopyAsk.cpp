#include "lib/messaging/CopyAsk.h"

#include "common/Utilities.h"
#include "common/ValueJson.h"

namespace pbr {
namespace copy_ask {

namespace {

/** How long an ask may wait in the relay and still be answered. */
constexpr int64_t kCopyAskMaxAgeMs = 60 * 60 * 1000;

} // namespace

Roe<ThreadMessage> BuildNotice(const CopyAsk& ask) {
  if (ask.message_id.empty() || ask.to_device.empty() || ask.from_device.empty()) {
    return Error("copy ask: a message, the device that sent it and ours");
  }
  Object detail;
  detail.set("message_id", ask.message_id);
  if (!ask.group_id.empty()) {
    detail.set("group_id", ask.group_id);
  }
  detail.set("to_device", ask.to_device);
  detail.set("from_device", ask.from_device);
  Object payload;
  payload.set("control_type", kCopyAskControlType);
  payload.set("detail", DumpJson(detail));  // a system payload keeps only its type and one detail string
  ThreadMessage notice;
  notice.id = util::GenerateUuid();
  notice.content_type = ChatContentType::System;
  notice.text = "Copy ask";
  notice.payload_json = DumpJson(payload);
  notice.timestamp = util::NowUnixMs();
  return notice;
}

Roe<std::optional<CopyAsk>> Parse(const ThreadMessage& notice) {
  if (notice.content_type != ChatContentType::System) {
    return std::optional<CopyAsk>{};
  }
  auto payload = TryParseObject(notice.payload_json);
  if (!payload || payload->getString("control_type").value_or("") != kCopyAskControlType) {
    return std::optional<CopyAsk>{};
  }
  auto detail = TryParseObject(payload->getString("detail").value_or(""));
  if (!detail) {
    return Error("copy ask: malformed");
  }
  CopyAsk ask;
  ask.message_id = detail->getString("message_id").value_or("");
  ask.group_id = detail->getString("group_id").value_or("");
  ask.to_device = detail->getString("to_device").value_or("");
  ask.from_device = detail->getString("from_device").value_or("");
  if (ask.message_id.empty() || ask.to_device.empty() || ask.from_device.empty()) {
    return Error("copy ask: malformed");
  }
  return std::optional<CopyAsk>(std::move(ask));
}

SystemControlFamily MakeFamily(std::function<void(CopyAsk ask, std::string asker_account)> answer) {
  const auto is_ask = [](const ThreadMessage& message) {
    auto parsed = Parse(message);
    return parsed && parsed->has_value();
  };
  SystemControlFamily family;
  family.name = kCopyAskFamily;
  family.owns = is_ask;
  family.on_lane = is_ask;
  family.lane_max_age_ms = kCopyAskMaxAgeMs;
  family.apply = [answer = std::move(answer)](ThreadMessage& message, const SystemControlContext& context) -> Roe<void> {
    auto parsed = Parse(message);
    if (!parsed) {
      return parsed.error();
    }
    if (*parsed && answer && !context.sender_identity.empty()) {
      answer(std::move(**parsed), context.sender_identity);
    }
    return {};
  };
  return family;
}

} // namespace copy_ask
} // namespace pbr
