#pragma once

#include "domain/messaging/CallTypes.h"
#include "common/thread/ThreadTypes.h"

#include "common/Error.h"

#include <optional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/** Encode/decode call signaling system payloads (detail JSON in ChatPayload). */
class CallControlCodec {
public:
  static Roe<std::string> EncodeInvite(const CallInviteDetail& detail);
  static Roe<CallInviteDetail> DecodeInvite(const std::string& detail_json);

  static Roe<std::string> EncodeAccept(const CallAcceptDetail& detail);
  static Roe<CallAcceptDetail> DecodeAccept(const std::string& detail_json);

  static Roe<std::string> EncodeDecline(const CallDeclineDetail& detail);
  static Roe<CallDeclineDetail> DecodeDecline(const std::string& detail_json);

  static Roe<std::string> EncodeLeave(const CallLeaveDetail& detail);
  static Roe<CallLeaveDetail> DecodeLeave(const std::string& detail_json);

  static Roe<std::string> EncodeRoster(const CallRosterDetail& detail);
  static Roe<CallRosterDetail> DecodeRoster(const std::string& detail_json);

  static Roe<std::string> EncodeMediaKey(const CallMediaKeyDetail& detail);
  static Roe<CallMediaKeyDetail> DecodeMediaKey(const std::string& detail_json);

  static Roe<std::string> EncodeEnded(const CallEndedDetail& detail);
  static Roe<CallEndedDetail> DecodeEnded(const std::string& detail_json);

  static Roe<std::string> EncodeStarted(const CallStartedDetail& detail);
  static Roe<CallStartedDetail> DecodeStarted(const std::string& detail_json);

  static Roe<std::string> EncodeSdp(const CallSdpDetail& detail);
  static Roe<CallSdpDetail> DecodeSdp(const std::string& detail_json);

  static Roe<std::string> EncodeIce(const CallIceDetail& detail);
  static Roe<CallIceDetail> DecodeIce(const std::string& detail_json);

  static Roe<std::string> EncodeSfuAttach(const CallSfuAttachDetail& detail);
  static Roe<CallSfuAttachDetail> DecodeSfuAttach(const std::string& detail_json);

  static Roe<std::string> EncodeSfuAttachFailed(const CallSfuAttachFailedDetail& detail);
  static Roe<CallSfuAttachFailedDetail> DecodeSfuAttachFailed(const std::string& detail_json);

  static Roe<std::string> EncodeHopRefuse(const CallHopRefuseDetail& detail);
  static Roe<CallHopRefuseDetail> DecodeHopRefuse(const std::string& detail_json);

  static Roe<std::string> EncodeVideoRefresh(const CallVideoRefreshDetail& detail);
  static Roe<CallVideoRefreshDetail> DecodeVideoRefresh(const std::string& detail_json);

  static Roe<ThreadMessage> BuildSystemMessage(const std::string& thread_id, CallControlType type,
                                               const std::string& display_text, const std::string& detail_json,
                                               const std::string& sender_contact_id);

  static std::optional<CallControlType> ControlTypeFromMessage(const ThreadMessage& message);
  static bool IsCallControlMessage(const ThreadMessage& message);
};

} // namespace pbr
