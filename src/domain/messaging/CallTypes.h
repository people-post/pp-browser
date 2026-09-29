#pragma once

#include "domain/messaging/CallMobility.h"

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

/** Group SoftMigrate vs subscribe-only live broadcast (B001 / B0). */
/**
 * Persisted session kind. `Broadcast` is read-only legacy (rows written before broadcast became its
 * own feature, media-client-layers l6): decoded so old stores load, never created, never accepted.
 */
enum class CallSessionKind : uint8_t { Group = 0, Broadcast = 1 };

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
  /** Guest → owner: SFU attach failed; optional preferred durable hops (V029). */
  CallSfuAttachFailed,
  /** Owner → guest: cannot place guest on a shared hop; leave with friendly copy (V029). */
  CallHopRefuse,
  /** Receiver asks publisher for an IDR (V034). */
  CallVideoRefresh,
  /**
   * H011 L3.1c: dialer announces chosen immediate circuit R1 PeerId so answerer can late-reserve.
   * Additive plumbing — old peers ignore unknown control_type.
   */
  CallCircuitR1,
  /**
   * H012 / L3.25d: ACP punch candidates + sync window over call-control when Amp introducer
   * is unavailable (B29). Additive — old peers ignore.
   */
  CallPunchOffer,
  CallPunchAnswer,
  /**
   * call-path-resilience K005: mid-call caps change (mobility class flipped). Additive — old peers
   * ignore it and keep the caps from invite / accept.
   */
  CallCapsUpdate,
};

struct CallParticipantMedia {
  bool audio_muted = false;
  bool video_enabled = false;
};

/**
 * V050 planned hop: the media_relay the initiator picked at StartCall from the invite list. Not
 * attached until the third join; carried in CallInvite so invitees can check it while ringing.
 */
struct CallPlannedHop {
  std::string peer_id;
  std::string multiaddr;
};

struct CallSession {
  std::string call_id;
  std::optional<std::string> origin_thread_id;
  std::optional<std::string> origin_group_id;
  CallMediaMode media_mode = CallMediaMode::Voice;
  /** Initiator policy: participants may publish video when true (V035). */
  bool video_allowed = false;
  CallSessionState state = CallSessionState::Ringing;
  int64_t created_at = 0;
  std::optional<int64_t> ended_at;
  uint32_t media_epoch = 1;
  std::string media_key_id;
  std::optional<std::string> sfu_hint;
  CallSessionKind session_kind = CallSessionKind::Group;
  /** V050: hop planned from the invite list (not the hop the call is on — that is `sfu_hint`). */
  std::optional<CallPlannedHop> planned_hop;
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
  bool video_allowed = false;
  std::optional<std::string> origin_thread_id;
  std::optional<std::string> origin_group_id;
  std::optional<std::string> sfu_hint;
  std::optional<int64_t> expires_at;
  int64_t created_at = 0;
  /** pending | accepted | declined | expired | missed */
  std::string status = "pending";
  CallSessionKind session_kind = CallSessionKind::Group;
};

struct CallRosterEntry {
  std::string identity;
  CallParticipantState state = CallParticipantState::Joined;
  bool audio_muted = false;
  bool video_enabled = false;
  /** Sticky SoftMigrate initiator (earliest wins). Optional for wire compat with older peers. */
  std::optional<int64_t> joined_at;
};

/**
 * Additive peer capability advertisement on call invite/accept (V030).
 * `v` bumps only on semantic break; unknown newer `v` → treat as unusable for hop pick.
 * Missing `caps` on wire → old peer (present=false); SoftMigrate fail-closed for contacts.
 */
inline constexpr int kCallPeerCapsVersion = 1;

struct CallPeerCaps {
  int v = kCallPeerCapsVersion;
  /** Durable media_relay host (Node + capability + started) — not ephemeral listen-only. */
  bool media_relay = false;
  /** K005: optional `mobility` — no `v` bump; missing / unrecognized reads as Unknown. */
  MobilityClass mobility = MobilityClass::Unknown;
  /** True when the `caps` object was present on wire. */
  bool present = false;
};

struct CallInviteDetail {
  std::string call_id;
  std::string inviter_identity;
  std::string invitee_identity;
  CallMediaMode media_mode = CallMediaMode::Voice;
  bool video_allowed = false;
  std::optional<std::string> origin_thread_id;
  std::optional<std::string> origin_group_id;
  std::optional<std::string> sfu_hint;
  /** V050: planned hop (additive; old peers ignore). Distinct from `sfu_hint` = the hop in use. */
  std::optional<CallPlannedHop> planned_hop;
  std::optional<int64_t> expires_at;
  /** Full call roster at invite time: joined + ringing + every co-invitee (V050) + this invitee. */
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
  /**
   * Inviter mesh PeerId (base58). Additive — lets peers map inbound call-media PeerId → relay:
   * without contacts or listen multiaddrs (group members need not be contacts).
   */
  std::string libp2p_peer_id;
  /** Optional capability ads (additive; old peers ignore). */
  CallPeerCaps caps;
  /**
   * Initiation offer (P001): pp_credit minor units the caller prepared before dial.
   * Missing on wire → 0 (free / old peer).
   */
  int64_t offer_amount_minor = 0;
  /** Peer's advertised floor used when preparing the offer (informational). */
  int64_t floor_minor = 0;
  std::string currency = "pp_credit";
};

/**
 * V050 gt4: what an invitee found while ringing, carried in CallAccept (additive; old peers send
 * none). `planned_hop_ok` unset = unknown (no plan, or the probe had not finished).
 */
struct CallHopReport {
  std::optional<bool> planned_hop_ok;
  /** Hops (PeerIds) this invitee reached (media_relay quote ok) — the planned one included when ok. */
  std::vector<std::string> reachable_hops;
  /** Hops it tried and could not reach. A hop still being probed is in neither list (unknown). */
  std::vector<std::string> unreachable_hops;
};

struct CallAcceptDetail {
  std::string call_id;
  std::string identity;
  bool audio_muted = false;
  bool video_enabled = false;
  /** Answerer listen multiaddrs so offerer can fallback-dial when inbound never arrives. */
  std::vector<std::string> listen_multiaddrs;
  /** Answerer mesh PeerId (base58); same role as invite.libp2p_peer_id. */
  std::string libp2p_peer_id;
  /** Optional capability ads (additive; old peers ignore). */
  CallPeerCaps caps;
  /**
   * Recipient charge decision for initiation offer (P001).
   * "waive" | "take_all"; missing → waive (compat / free path).
   */
  std::string charge_decision = "waive";
  /** Echo of offer amount being waived or taken. */
  int64_t offer_amount_minor = 0;
  /** Callee's answer mode: absent = unchanged (old peers, video answer); false = answered as voice
   *  only — a 1:1 caller narrows the call to voice. Never true on the wire. */
  std::optional<bool> video_allowed;
  /** V050 gt4: planned-hop reachability + reachable hops. */
  CallHopReport hop_report;
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
  bool video_allowed = false;
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

/** Guest could not attach to owner-picked hop (V029). */
struct CallSfuAttachFailedDetail {
  std::string call_id;
  std::string identity;
  std::string failed_hop_peer_id;
  std::string error;
  /** Ordered PeerIds the guest can dial (durable/seed); owner intersects with its rank. */
  std::vector<std::string> preferred_hop_peer_ids;
};

/** Owner refuses guest after hop-hint intersection empty (V029). */
struct CallHopRefuseDetail {
  std::string call_id;
  std::string identity;
  /** Stable reason id for logs (`no_shared_hop`). */
  std::string reason;
  /** User-facing sentence (already localized by sender). */
  std::string message;
};

/** Receiver asks publisher for an IDR (V034). */
struct CallVideoRefreshDetail {
  std::string call_id;
  /** Publisher identity whose video_lo should keyframe. Empty = local publisher. */
  std::string identity;
};

/**
 * H011 L3.1c: dialer → answerer chosen immediate relay PeerId (no multiaddrs — H007 carve-out).
 */
struct CallCircuitR1Detail {
  std::string call_id;
  /** Immediate circuit relay PeerId (base58). */
  std::string circuit_r1;
};

/**
 * H012: punch candidate exchange over call-control (same collect as H009, no Amp introducer Session).
 */
/** K005: the sender's caps changed during the call (its mobility class flipped). */
struct CallCapsUpdateDetail {
  std::string call_id;
  std::string identity;
  CallPeerCaps caps;
};

struct CallPunchDetail {
  std::string call_id;
  std::string epoch_id;
  int window_ms = 2000;
  std::vector<std::string> addrs;
  /** Mesh PeerId of the sender (offer: initiator; answer: target). */
  std::string peer_id;
};

std::string GenerateCallId();
std::string CallMediaModeToString(CallMediaMode mode);
CallMediaMode CallMediaModeFromString(const std::string& value);
std::string CallSessionKindToString(CallSessionKind kind);
CallSessionKind CallSessionKindFromString(const std::string& value);
inline bool IsBroadcastSession(CallSessionKind kind) { return kind == CallSessionKind::Broadcast; }
std::string CallSessionStateToString(CallSessionState state);
CallSessionState CallSessionStateFromString(const std::string& value);
std::string CallParticipantStateToString(CallParticipantState state);
CallParticipantState CallParticipantStateFromString(const std::string& value);
std::string CallControlTypeToWire(CallControlType type);
std::optional<CallControlType> CallControlTypeFromWire(const std::string& value);

} // namespace pbr
