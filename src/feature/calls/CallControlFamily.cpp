#include "feature/calls/CallControlFamily.h"

#include "domain/calls/CallControlCodec.h"
#include "foundation/i18n/LocalizationService.h"

#include <cstdio>
#include <map>
#include <utility>
#include "common/ValueJson.h"
#include "common/PbrCompat.h"

namespace pbr {

namespace {

/** How long call signaling may wait in the relay: an invite rings for a minute; slack for delivery. */
constexpr int64_t kCallLaneMaxAgeMs = 5 * 60 * 1000;

std::optional<std::string> DetailJson(const ThreadMessage& message) {
  auto payload = TryParseObject(message.payload_json);
  if (!payload) {
    return std::nullopt;
  }
  return payload->getString("detail");
}

std::optional<CallSummaryDetail> SummaryOf(const ThreadMessage& message) {
  const auto type = CallControlCodec::ControlTypeFromMessage(message);
  if (!type || *type != CallControlType::CallSummary) {
    return std::nullopt;
  }
  const auto detail = DetailJson(message);
  if (!detail) {
    return std::nullopt;
  }
  auto summary = CallControlCodec::DecodeSummary(*detail);
  if (!summary) {
    return std::nullopt;
  }
  return std::move(summary.value());
}

std::string FormatDurationMs(const int64_t duration_ms) {
  if (duration_ms < 0) {
    return {};
  }
  const int64_t total_sec = duration_ms / 1000;
  const int64_t hours = total_sec / 3600;
  const int64_t mins = (total_sec % 3600) / 60;
  const int64_t secs = total_sec % 60;
  char buf[32];
  if (hours > 0) {
    std::snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld", static_cast<long long>(hours),
                  static_cast<long long>(mins), static_cast<long long>(secs));
  } else {
    std::snprintf(buf, sizeof(buf), "%lld:%02lld", static_cast<long long>(mins), static_cast<long long>(secs));
  }
  return buf;
}

std::string PeerLabel(const SystemControlLabels& labels, const std::string& identity) {
  if (identity.empty()) {
    return Tr("call.label.someone");
  }
  if (identity == kLocalSelfContactId) {
    return Tr("call.label.you");
  }
  const std::string label = labels.peer ? labels.peer(identity) : std::string();
  return label.empty() ? Tr("call.label.someone") : label;
}

} // namespace

std::optional<std::string> CallSummaryLine(const ThreadMessage& message, const SystemControlLabels& labels) {
  const auto summary = SummaryOf(message);
  if (!summary) {
    return std::nullopt;
  }
  const std::string media = summary->media_mode == CallMediaMode::Video ? "_video" : "_voice";
  const bool ours = message.sender_contact_id == kLocalSelfContactId;
  const std::string duration = summary->duration_ms ? FormatDurationMs(*summary->duration_ms) : std::string();
  auto line = [&](const std::string& key, std::map<std::string, std::string> args = {}) {
    return Tr("call.summary." + key + media, args);
  };

  if (summary->group) {
    const std::string starter = PeerLabel(labels, message.sender_contact_id);
    if (summary->outcome == CallOutcome::Completed && !duration.empty()) {
      return line("group_duration", {{"name", starter}, {"duration", duration}});
    }
    return line("group", {{"name", starter}});
  }
  switch (summary->outcome) {
  case CallOutcome::Completed:
    return line(ours ? "outgoing" : "incoming", {{"duration", duration}});
  case CallOutcome::Missed:
    return line(ours ? "no_answer" : "missed");
  case CallOutcome::Cancelled:
    return line(ours ? "cancelled" : "missed");
  case CallOutcome::Declined:
    return line(ours ? "declined_by_peer" : "declined");
  case CallOutcome::Failed:
    return line("failed");
  }
  return line("failed");
}

bool CallSummaryIsNews(const ThreadMessage& message) {
  const auto summary = SummaryOf(message);
  if (!summary || summary->group || message.sender_contact_id == kLocalSelfContactId) {
    return false;
  }
  return summary->outcome == CallOutcome::Missed || summary->outcome == CallOutcome::Cancelled;
}

SystemControlFamily MakeCallControlFamily(CallControlFamilyPorts ports) {
  SystemControlFamily family;
  family.name = "call";
  family.owns = [](const ThreadMessage& message) { return CallControlCodec::IsCallControlMessage(message); };
  family.apply = [receive = ports.receive](ThreadMessage& message, const SystemControlContext& context) -> Roe<void> {
    const auto type = CallControlCodec::ControlTypeFromMessage(message);
    if (type && *type == CallControlType::CallSummary) {
      return {};  // history: nothing to do
    }
    if (receive) {
      receive(message, context);
    }
    return {};
  };
  family.on_lane = [](const ThreadMessage& message) {
    const auto type = CallControlCodec::ControlTypeFromMessage(message);
    return type && CallControlCodec::TravelsOnLane(*type);
  };
  family.lane_max_age_ms = kCallLaneMaxAgeMs;
  family.quiet = [](const ThreadMessage& message) { return !CallSummaryIsNews(message); };
  family.line = [](const ThreadMessage& message, const SystemControlLabels& labels) {
    return CallSummaryLine(message, labels);
  };
  family.live = std::move(ports.has_active_local_call);
  return family;
}

} // namespace pbr
