#pragma once

#include "domain/messaging/CallTypes.h"
#include "common/thread/ThreadTypes.h"

#include <string>
#include <vector>

namespace pbr {

/**
 * True when `thread` is an e2e_public Direct and another Direct with the same peer uses
 * private e2e — the public row is the call/group control twin.
 */
bool HasPrivateE2eSibling(const Thread& thread, const std::vector<Thread>& all_threads);

/**
 * True when `thread` looks like an e2e_public twin created for call signaling while the
 * user already has a private (e2e) Direct with the same peer. Empty preview + no unread —
 * hide from the sessions list so call control does not grow a second DM.
 */
bool IsCallControlShadowThread(const Thread& thread, const std::vector<Thread>& all_threads);

/**
 * Active call touches this thread: origin_thread_id match, or Direct peer is Joined
 * (joined_participants is typically ListJoinedParticipants), or Group origin_group_id match.
 */
bool ThreadMatchesActiveCall(const Thread& thread, const CallSession& session,
                             const std::vector<CallParticipant>& joined_participants);

} // namespace pbr
