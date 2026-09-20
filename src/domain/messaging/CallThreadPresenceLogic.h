#pragma once

#include "domain/messaging/CallTypes.h"
#include "common/thread/ThreadTypes.h"

#include <cstddef>
#include <string>
#include <vector>

namespace pbr {

/** Max messages scanned when deciding an e2e_public twin is call-control-only (GC). */
inline constexpr size_t kOrphanCallControlShadowScanLimit = 64;

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
 * True when every message is call-control plumbing/signaling, and the page is complete
 * (`messages.size() < scan_limit`). Empty transcript is allowed. Hitting the scan limit
 * means we cannot prove the older history is call-only — return false.
 */
bool TranscriptIsOnlyCallControl(const std::vector<ThreadMessage>& messages, size_t scan_limit);

/**
 * Active call touches this thread: origin_thread_id match, or Direct peer is Joined
 * (joined_participants is typically ListJoinedParticipants), or Group origin_group_id match.
 */
bool ThreadMatchesActiveCall(const Thread& thread, const CallSession& session,
                             const std::vector<CallParticipant>& joined_participants);

} // namespace pbr
