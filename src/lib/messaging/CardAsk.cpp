#include "lib/messaging/CardAsk.h"

#include "common/Utilities.h"
#include "common/ValueJson.h"

namespace pbr {
namespace card_ask {

namespace {

/** An ask waiting longer than this in the relay is stale: the asker will have seen another hint by then. */
constexpr int64_t kCardAskMaxAgeMs = 60 * 60 * 1000;

} // namespace

Roe<ThreadMessage> BuildNotice(const std::string& to_device) {
  if (to_device.empty()) {
    return Error("card ask: no device");
  }
  Object detail;
  detail.set("to_device", to_device);
  Object payload;
  payload.set("control_type", kCardAskControlType);
  payload.set("detail", DumpJson(detail));  // a system payload keeps only its type and one detail string
  ThreadMessage notice;
  notice.id = util::GenerateUuid();
  notice.content_type = ChatContentType::System;
  notice.text = "Card ask";
  notice.payload_json = DumpJson(payload);
  notice.timestamp = util::NowUnixMs();
  return notice;
}

Roe<std::optional<std::string>> Parse(const ThreadMessage& notice) {
  if (notice.content_type != ChatContentType::System) {
    return std::optional<std::string>{};
  }
  auto payload = TryParseObject(notice.payload_json);
  if (!payload || payload->getString("control_type").value_or("") != kCardAskControlType) {
    return std::optional<std::string>{};
  }
  auto detail = TryParseObject(payload->getString("detail").value_or(""));
  const std::string to_device = detail ? detail->getString("to_device").value_or("") : std::string();
  if (to_device.empty()) {
    return Error("card ask: malformed");
  }
  return std::optional<std::string>(to_device);
}

SystemControlFamily MakeFamily(std::function<void(std::string to_device, std::string asker_account)> answer) {
  const auto is_ask = [](const ThreadMessage& message) {
    auto parsed = Parse(message);
    return parsed && parsed->has_value();
  };
  SystemControlFamily family;
  family.name = kCardAskFamily;
  family.owns = is_ask;
  family.on_lane = is_ask;
  family.lane_max_age_ms = kCardAskMaxAgeMs;
  family.apply = [answer = std::move(answer)](ThreadMessage& message, const SystemControlContext& context) -> Roe<void> {
    auto parsed = Parse(message);
    if (!parsed) {
      return parsed.error();
    }
    if (*parsed && answer && !context.sender_identity.empty()) {
      answer(**parsed, context.sender_identity);
    }
    return {};
  };
  return family;
}

} // namespace card_ask
} // namespace pbr
