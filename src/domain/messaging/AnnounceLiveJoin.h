#pragma once

#include "domain/messaging/PeerAnnounceTypes.h"
#include "domain/messaging/BroadcastJoinTicket.h"

#include "common/Error.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {

/**
 * Spine C (slice 0): plan joining a live program from a tip.
 * `call_id` is tip.join_handle (opaque call/session id). No SoftMigrate / media yet.
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

} // namespace pbr
