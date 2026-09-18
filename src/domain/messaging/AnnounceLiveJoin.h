#pragma once

#include "domain/messaging/BroadcastJoinTicket.h"
#include "domain/messaging/CallTypes.h"
#include "domain/messaging/PeerAnnounceTypes.h"

#include "common/Error.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {

/**
 * Spine C: plan joining a live program from a tip, then materialize pending invite + ringing
 * session for AcceptInvite. No SoftMigrate / media attach here.
 * `call_id` is tip.join_handle (opaque call/session id).
 */
struct AnnounceLiveJoinPlan {
  std::string call_id;
  std::string publisher_peer_id;
  std::string topic_id;
  std::string program_id;
  /** Optional media_relay hop PeerId (tip.hop_peer_id → session.sfu_hint). */
  std::string hop_peer_id;
  /** B007 L1 hints from tip; hop_peer_id is primary dial (explicit or first L1). */
  std::vector<std::string> l1_hop_peer_ids;
  /** Stable broadcast media epoch after join-ticket apply (B0); default 1. */
  uint32_t media_epoch = 1;
  /** Opaque media_key_id from CallMediaKeyStore after ticket apply (optional). */
  std::string media_key_id;
  uint64_t seq = 0;
  uint64_t epoch = 0;
};

/** True when tip is Live and join_handle is non-empty. */
bool TipIsLiveJoinable(const PeerAnnounceTip& tip);

/**
 * Plan a realtime join from a tip. Rejects Scheduled/Ended/empty join_handle.
 * Does not dial SoftMigrate or attach media.
 */

/** Optional B0 ticket apply when arming a live-announce join. */
struct ArmLiveAnnounceJoinOpts {
  const BroadcastJoinTicket* ticket = nullptr;
  const ByteVector* publisher_mldsa_public_key = nullptr;
  const ByteVector* viewer_pairwise_session_key = nullptr;
  /** 0 → use NowUnixMs(). */
  int64_t now_ms = 0;
};

Roe<AnnounceLiveJoinPlan> PlanAnnounceLiveJoin(const PeerAnnounceTip& tip);

/** Pending invite + ringing session rows for AcceptInvite. */
struct AnnounceLiveJoinHandoff {
  PendingCallInvite pending;
  CallSession session;
};

/**
 * Build invite/session rows for `local_invitee_identity`.
 * `inviter_identity` should be the publisher's call identity (Account when known,
 * else PeerId). `video_allowed` defaults true for live broadcast tips.
 */
Roe<AnnounceLiveJoinHandoff> BuildAnnounceLiveJoinHandoff(const AnnounceLiveJoinPlan& plan,
                                                          std::string_view local_invitee_identity,
                                                          std::string_view inviter_identity,
                                                          int64_t now_ms,
                                                          bool video_allowed = true);

} // namespace pbr
