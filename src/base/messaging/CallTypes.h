#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

inline constexpr const char* kCallIdPrefix = "call:";
/** Engineering floor until SFU load proves out (V007). */
inline constexpr size_t kCallEngineeringMaxJoined = 8;
/** Soft protocol max (V007). */
inline constexpr size_t kCallProtocolSoftMaxJoined = 16;
/** Default invite ring TTL. */
inline constexpr int64_t kDefaultCallInviteTtlMs = 60'000;
/** Slack added to relay-age gate (clock granularity / brief queue delay). */
inline constexpr int64_t kCallInviteRelayAgeSlackMs = 5'000;
/**
 * Without relay create/now samples (direct delivery / old servers), allow wire expires_at
 * to be this far past local now before dropping (clock skew). Beyond this, do not re-arm.
 */
inline constexpr int64_t kCallInviteWireSkewSlackMs = 120'000;

enum class CallMediaMode : uint8_t { Voice = 0, Video = 1 };

enum class CallSessionState : uint8_t { Ringing = 0, Active = 1, Ended = 2 };

enum class CallParticipantState : uint8_t {
  Invited = 0,
  Ringing = 1,
  Joined = 2,
  Left = 3,
  Declined = 4,
  Missed = 5,
};

enum class CallControlType {
  CallInvite,
  CallAccept,
  CallDecline,
  CallLeave,
  CallRoster,
  CallMediaKey,
  CallEnded,
  /** Origin-thread history only (local); not required on pairwise wire. */
  CallStarted,
  /** WebRTC SDP offer/answer (a2). */
  CallSdp,
  /** Trickle ICE candidate (a2). */
  CallIce,
  /** Soft-migrate onto media_relay hop (a4): hop peer id + stream map hint. */
  CallSfuAttach,
};

struct CallParticipantMedia {
  bool audio_muted = false;
  bool video_enabled = false;
};

struct CallSession {
  std::string call_id;
  std::optional<std::string> origin_thread_id;
  std::optional<std::string> origin_group_id;
  CallMediaMode media_mode = CallMediaMode::Voice;
  CallSessionState state = CallSessionState::Ringing;
  int64_t created_at = 0;
  std::optional<int64_t> ended_at;
  uint32_t media_epoch = 1;
  std::string media_key_id;
  std::optional<std::string> sfu_hint;
};

struct CallParticipant {
  std::string call_id;
  std::string identity;
  CallParticipantState state = CallParticipantState::Invited;
  CallParticipantMedia media;
  std::optional<int64_t> joined_at;
  std::optional<int64_t> left_at;
};

struct PendingCallInvite {
  std::string call_id;
  std::string inviter_identity;
  std::string invitee_identity;
  CallMediaMode media_mode = CallMediaMode::Voice;
  std::optional<std::string> origin_thread_id;
  std::optional<std::string> origin_group_id;
  std::optional<std::string> sfu_hint;
  std::optional<int64_t> expires_at;
  int64_t created_at = 0;
  /** pending | accepted | declined | expired | missed */
  std::string status = "pending";
};

struct CallRosterEntry {
  std::string identity;
  CallParticipantState state = CallParticipantState::Joined;
  bool audio_muted = false;
  bool video_enabled = false;
};

struct CallInviteDetail {
  std::string call_id;
  std::string inviter_identity;
  std::string invitee_identity;
  CallMediaMode media_mode = CallMediaMode::Voice;
  std::optional<std::string> origin_thread_id;
  std::optional<std::string> origin_group_id;
  std::optional<std::string> sfu_hint;
  std::optional<int64_t> expires_at;
  /** Full call roster snapshot at invite time (joined + ringing + this invitee). */
  std::vector<CallRosterEntry> participants;
  /** Optional epoch-1 media key (same fields as CallMediaKey) so Accept need not wait on a second inbox row. */
  uint32_t media_epoch = 1;
  std::string media_key_id;
  std::string wrapped_key_b64;
  /**
   * Inviter LAN listen multiaddrs (`/ip4/…/tcp/…/p2p/…`) for answerer reverse-dial when mDNS
   * has not populated the dial registry yet (desktop cross-OS dogfood).
   */
  std::vector<std::string> listen_multiaddrs;
};

struct CallAcceptDetail {
  std::string call_id;
  std::string identity;
  bool audio_muted = false;
  bool video_enabled = false;
  /** Answerer listen multiaddrs so offerer can fallback-dial when inbound never arrives. */
  std::vector<std::string> listen_multiaddrs;
};

struct CallDeclineDetail {
  std::string call_id;
  std::string identity;
};

struct CallLeaveDetail {
  std::string call_id;
  std::string identity;
};

struct CallRosterDetail {
  std::string call_id;
  uint32_t media_epoch = 1;
  std::vector<CallRosterEntry> participants;
};

struct CallMediaKeyDetail {
  std::string call_id;
  uint32_t media_epoch = 1;
  std::string media_key_id;
  /** Opaque wrapped key material (base64); empty until crypto wiring. */
  std::string wrapped_key_b64;
};

struct CallEndedDetail {
  std::string call_id;
  std::optional<int64_t> duration_ms;
};

struct CallStartedDetail {
  std::string call_id;
  CallMediaMode media_mode = CallMediaMode::Voice;
};

struct CallSdpDetail {
  std::string call_id;
  std::string identity;
  /** "offer" | "answer" */
  std::string sdp_type;
  std::string sdp;
};

struct CallIceDetail {
  std::string call_id;
  std::string identity;
  std::string candidate;
  std::string mid;
};

/** a4 soft-migrate onto blind media_relay (V021). */
struct CallSfuAttachDetail {
  std::string call_id;
  /** Hop PeerId (session key / multiaddr /p2p/ id). */
  std::string hop_peer_id;
  std::string hop_multiaddr;
  std::string quote_id;
  /** Local publisher stream_id for this sender (others subscribe). */
  uint32_t publisher_stream_id = 0;
};

std::string GenerateCallId();
std::string CallMediaModeToString(CallMediaMode mode);
CallMediaMode CallMediaModeFromString(const std::string& value);
std::string CallSessionStateToString(CallSessionState state);
CallSessionState CallSessionStateFromString(const std::string& value);
std::string CallParticipantStateToString(CallParticipantState state);
CallParticipantState CallParticipantStateFromString(const std::string& value);
std::string CallControlTypeToWire(CallControlType type);
std::optional<CallControlType> CallControlTypeFromWire(const std::string& value);

} // namespace pbr
