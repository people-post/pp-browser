#include "feature/calls/CallControlFamily.h"

#include "domain/calls/CallControlCodec.h"
#include "foundation/i18n/LocalizationService.h"

#include <cstdio>
#include <utility>
#include "common/ValueJson.h"
#include "common/PbrCompat.h"

namespace pbr {

namespace {

/** Shown in no transcript: media/path plumbing, and mid-call roster updates (every mute or camera
 *  change sends one — a line for each would only repeat who is in the call). */
bool HiddenType(const CallControlType type) {
  return CallControlCodec::IsPlumbingCallControl(type) || type == CallControlType::CallRoster;
}

std::optional<std::string> DetailJson(const ThreadMessage& message) {
  auto payload = TryParseObject(message.payload_json);
  if (!payload) {
    return std::nullopt;
  }
  return payload->getString("detail");
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

/** A detail decoded, or none (absent or unreadable). */
template <typename T, typename Decode>
std::optional<T> Decoded(const std::optional<std::string>& detail, Decode decode) {
  if (!detail) {
    return std::nullopt;
  }
  auto decoded = decode(*detail);
  if (!decoded) {
    return std::nullopt;
  }
  return std::move(decoded.value());
}

/** The person a joined / declined / left line is about: the payload's, else the sender. */
template <typename Decoded>
std::string ActorLabel(const ThreadMessage& message, const std::optional<Decoded>& decoded,
                       const SystemControlLabels& labels) {
  if (message.sender_contact_id == kLocalSelfContactId) {
    return Tr("call.label.you");
  }
  std::string identity = message.sender_contact_id;
  if (decoded && !decoded->identity.empty()) {
    identity = decoded->identity;
  }
  return PeerLabel(labels, identity);
}

} // namespace

std::optional<std::string> CallHistoryLine(const ThreadMessage& message, const CallControlType type,
                                           const SystemControlLabels& labels) {
  if (HiddenType(type)) {
    return std::nullopt;
  }
  const auto detail = DetailJson(message);
  const bool outbound = message.sender_contact_id == kLocalSelfContactId;
  switch (type) {
  case CallControlType::CallStarted: {
    CallMediaMode mode = CallMediaMode::Voice;
    if (detail) {
      if (auto started = CallControlCodec::DecodeStarted(*detail)) {
        mode = started->media_mode;
      }
    }
    return mode == CallMediaMode::Video ? Tr("call.history.started_video") : Tr("call.history.started_voice");
  }
  case CallControlType::CallInvite: {
    CallMediaMode mode = CallMediaMode::Voice;
    std::string peer;
    if (detail) {
      if (auto invite = CallControlCodec::DecodeInvite(*detail)) {
        mode = invite->media_mode;
        peer = outbound ? invite->invitee_identity : invite->inviter_identity;
      }
    }
    if (peer.empty() && !outbound) {
      peer = message.sender_contact_id;
    }
    const std::string name = PeerLabel(labels, peer);
    if (outbound) {
      return mode == CallMediaMode::Video ? Tr("call.history.outgoing_video", {{"name", name}})
                                          : Tr("call.history.outgoing_voice", {{"name", name}});
    }
    return mode == CallMediaMode::Video ? Tr("call.history.incoming_video", {{"name", name}})
                                        : Tr("call.history.incoming_voice", {{"name", name}});
  }
  case CallControlType::CallAccept: {
    const auto accept = Decoded<CallAcceptDetail>(detail, CallControlCodec::DecodeAccept);
    return Tr("call.history.joined", {{"name", ActorLabel(message, accept, labels)}});
  }
  case CallControlType::CallDecline: {
    const auto decline = Decoded<CallDeclineDetail>(detail, CallControlCodec::DecodeDecline);
    return Tr("call.history.declined", {{"name", ActorLabel(message, decline, labels)}});
  }
  case CallControlType::CallLeave: {
    const auto leave = Decoded<CallLeaveDetail>(detail, CallControlCodec::DecodeLeave);
    return Tr("call.history.left", {{"name", ActorLabel(message, leave, labels)}});
  }
  case CallControlType::CallEnded: {
    if (detail) {
      if (auto ended = CallControlCodec::DecodeEnded(*detail); ended && ended->duration_ms && *ended->duration_ms >= 0) {
        if (const std::string duration = FormatDurationMs(*ended->duration_ms); !duration.empty()) {
          return Tr("call.history.ended_duration", {{"duration", duration}});
        }
      }
    }
    return Tr("call.history.ended");
  }
  default:
    return message.text.empty() ? Tr("call.history.ended") : message.text;
  }
}

SystemControlFamily MakeCallControlFamily(CallControlFamilyPorts ports) {
  SystemControlFamily family;
  family.name = "call";
  family.owns = [](const ThreadMessage& message) { return CallControlCodec::IsCallControlMessage(message); };
  family.apply = [receive = ports.receive](ThreadMessage& message, const SystemControlContext& context) -> Roe<void> {
    if (receive) {
      receive(message, context);
    }
    return {};
  };
  family.apply_out_of_order = true;
  family.hidden = [](const ThreadMessage& message) {
    const auto type = CallControlCodec::ControlTypeFromMessage(message);
    return type && HiddenType(*type);
  };
  family.quiet = [](const ThreadMessage& message) {
    const auto type = CallControlCodec::ControlTypeFromMessage(message);
    return type && CallControlCodec::SuppressesInboxChrome(*type);
  };
  family.line = [](const ThreadMessage& message, const SystemControlLabels& labels) -> std::optional<std::string> {
    const auto type = CallControlCodec::ControlTypeFromMessage(message);
    return type ? CallHistoryLine(message, *type, labels) : std::nullopt;
  };
  family.live = std::move(ports.has_active_local_call);
  family.preferred_public_thread = std::move(ports.active_call_origin_thread_id);
  return family;
}

} // namespace pbr
