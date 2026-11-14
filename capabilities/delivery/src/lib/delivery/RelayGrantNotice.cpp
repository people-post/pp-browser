#include "lib/delivery/RelayGrantNotice.h"

#include "common/Utilities.h"
#include "common/ValueJson.h"
#include "common/chat/MessagingJson.h"

namespace pbr {
namespace relay_grant {

namespace {

/** A notice waiting longer than this in the relay is stale: the grantee asks again, the grantor sends again. */
constexpr int64_t kNoticeMaxAgeMs = 60 * 60 * 1000;

Roe<ThreadMessage> BuildNotice(const char* control_type, const Object& detail, const char* text) {
  Object payload;
  payload.set("control_type", control_type);
  payload.set("detail", DumpJson(detail)); // a system payload keeps only its type and one detail string
  ThreadMessage notice;
  notice.id = util::GenerateUuid();
  notice.content_type = ChatContentType::System;
  notice.text = text;
  notice.payload_json = DumpJson(payload);
  notice.timestamp = util::NowUnixMs();
  return notice;
}

} // namespace

Roe<ThreadMessage> BuildGrant(const RelayGrant& grant) {
  return BuildNotice(kGrantControlType, RelayGrantToJson(grant), "Relay grant");
}

Roe<ThreadMessage> BuildAck(const int64_t issued_at) {
  if (issued_at <= 0) {
    return Error("relay grant ack: no grant");
  }
  Object detail;
  detail.set("issued_at", issued_at);
  return BuildNotice(kAckControlType, detail, "Relay grant kept");
}

Roe<ThreadMessage> BuildAsk() {
  return BuildNotice(kAskControlType, Object{}, "Relay grant ask");
}

Roe<std::optional<Notice>> Parse(const ThreadMessage& message) {
  if (message.content_type != ChatContentType::System) {
    return std::optional<Notice>{};
  }
  auto payload = TryParseObject(message.payload_json);
  if (!payload) {
    return std::optional<Notice>{};
  }
  const std::string type = payload->getString("control_type").value_or("");
  if (type != kGrantControlType && type != kAckControlType && type != kAskControlType) {
    return std::optional<Notice>{};
  }
  auto detail = TryParseObject(payload->getString("detail").value_or(""));
  if (!detail) {
    return Error("relay grant notice: malformed");
  }
  Notice notice;
  if (type == kGrantControlType) {
    auto grant = RelayGrantFromJson(*detail);
    if (!grant) {
      return grant.error();
    }
    notice.kind = NoticeKind::Grant;
    notice.grant = std::move(*grant);
  } else if (type == kAckControlType) {
    notice.kind = NoticeKind::Ack;
    notice.issued_at = detail->getIf<int64_t>("issued_at").value_or(0);
    if (notice.issued_at <= 0) {
      return Error("relay grant ack: malformed");
    }
  } else {
    notice.kind = NoticeKind::Ask;
  }
  return std::optional<Notice>(std::move(notice));
}

SystemControlFamily MakeFamily(std::function<Roe<void>(const Notice& notice, const std::string& sender)> on_notice) {
  const auto is_notice = [](const ThreadMessage& message) {
    auto parsed = Parse(message);
    return parsed && parsed->has_value();
  };
  SystemControlFamily family;
  family.name = kFamily;
  family.owns = is_notice;
  family.on_lane = is_notice;
  family.lane_max_age_ms = kNoticeMaxAgeMs;
  family.apply = [on_notice = std::move(on_notice)](ThreadMessage& message,
                                                    const SystemControlContext& context) -> Roe<void> {
    auto parsed = Parse(message);
    if (!parsed) {
      return parsed.error();
    }
    if (!*parsed || !on_notice || context.sender_identity.empty()) {
      return {};
    }
    return on_notice(**parsed, context.sender_identity);
  };
  return family;
}

} // namespace relay_grant
} // namespace pbr
