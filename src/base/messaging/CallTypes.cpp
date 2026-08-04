#include "base/messaging/CallTypes.h"

#include "common/Utilities.h"

namespace pbr {

std::string GenerateCallId() {
  return std::string(kCallIdPrefix) + util::GenerateUuid();
}

std::string CallMediaModeToString(const CallMediaMode mode) {
  switch (mode) {
  case CallMediaMode::Voice:
    return "voice";
  case CallMediaMode::Video:
    return "video";
  }
  return "voice";
}

CallMediaMode CallMediaModeFromString(const std::string& value) {
  if (value == "video") {
    return CallMediaMode::Video;
  }
  return CallMediaMode::Voice;
}

std::string CallSessionStateToString(const CallSessionState state) {
  switch (state) {
  case CallSessionState::Ringing:
    return "ringing";
  case CallSessionState::Active:
    return "active";
  case CallSessionState::Ended:
    return "ended";
  }
  return "ringing";
}

CallSessionState CallSessionStateFromString(const std::string& value) {
  if (value == "active") {
    return CallSessionState::Active;
  }
  if (value == "ended") {
    return CallSessionState::Ended;
  }
  return CallSessionState::Ringing;
}

std::string CallParticipantStateToString(const CallParticipantState state) {
  switch (state) {
  case CallParticipantState::Invited:
    return "invited";
  case CallParticipantState::Ringing:
    return "ringing";
  case CallParticipantState::Joined:
    return "joined";
  case CallParticipantState::Left:
    return "left";
  case CallParticipantState::Declined:
    return "declined";
  case CallParticipantState::Missed:
    return "missed";
  }
  return "invited";
}

CallParticipantState CallParticipantStateFromString(const std::string& value) {
  if (value == "ringing") {
    return CallParticipantState::Ringing;
  }
  if (value == "joined") {
    return CallParticipantState::Joined;
  }
  if (value == "left") {
    return CallParticipantState::Left;
  }
  if (value == "declined") {
    return CallParticipantState::Declined;
  }
  if (value == "missed") {
    return CallParticipantState::Missed;
  }
  return CallParticipantState::Invited;
}

std::string CallControlTypeToWire(const CallControlType type) {
  switch (type) {
  case CallControlType::CallInvite:
    return "call_invite";
  case CallControlType::CallAccept:
    return "call_accept";
  case CallControlType::CallDecline:
    return "call_decline";
  case CallControlType::CallLeave:
    return "call_leave";
  case CallControlType::CallRoster:
    return "call_roster";
  case CallControlType::CallMediaKey:
    return "call_media_key";
  case CallControlType::CallEnded:
    return "call_ended";
  case CallControlType::CallStarted:
    return "call_started";
  case CallControlType::CallSdp:
    return "call_sdp";
  case CallControlType::CallIce:
    return "call_ice";
  case CallControlType::CallSfuAttach:
    return "call_sfu_attach";
  case CallControlType::CallSfuAttachFailed:
    return "call_sfu_attach_failed";
  case CallControlType::CallHopRefuse:
    return "call_hop_refuse";
  }
  return "call_invite";
}

std::optional<CallControlType> CallControlTypeFromWire(const std::string& value) {
  if (value == "call_invite") {
    return CallControlType::CallInvite;
  }
  if (value == "call_accept" || value == "call_join") {
    return CallControlType::CallAccept;
  }
  if (value == "call_decline") {
    return CallControlType::CallDecline;
  }
  if (value == "call_leave") {
    return CallControlType::CallLeave;
  }
  if (value == "call_roster") {
    return CallControlType::CallRoster;
  }
  if (value == "call_media_key") {
    return CallControlType::CallMediaKey;
  }
  if (value == "call_ended") {
    return CallControlType::CallEnded;
  }
  if (value == "call_started") {
    return CallControlType::CallStarted;
  }
  if (value == "call_sdp") {
    return CallControlType::CallSdp;
  }
  if (value == "call_ice") {
    return CallControlType::CallIce;
  }
  if (value == "call_sfu_attach") {
    return CallControlType::CallSfuAttach;
  }
  if (value == "call_sfu_attach_failed") {
    return CallControlType::CallSfuAttachFailed;
  }
  if (value == "call_hop_refuse") {
    return CallControlType::CallHopRefuse;
  }
  return std::nullopt;
}

} // namespace pbr
