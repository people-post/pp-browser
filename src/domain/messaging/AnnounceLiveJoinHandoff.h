#pragma once

#include "domain/messaging/AnnounceLiveJoin.h"
#include "domain/messaging/CallTypes.h"

#include "common/Error.h"

#include <cstdint>
#include <string>
#include <string_view>

#include "common/PbrCompat.h"

namespace pbr {

/**
 * Spine C (slice 1): materialize a pending call invite + ringing session from a
 * live-join plan so AcceptInvite can run later. No SoftMigrate / media attach.
 */
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
