#pragma once

#include "common/thread/IThreadStore.h"
#include "common/thread/ThreadTypes.h"
#include "common/Error.h"
#include "common/PbrCompat.h"

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace pbr {

/**
 * Public direct twins: control messages (call control, …) go to a peer on its e2e_public Direct,
 * beside the private e2e Direct the user chats in. These helpers find, hide and prune those twins.
 */

/** Max messages scanned when deciding an e2e_public twin carries only control (GC). */
inline constexpr size_t kOrphanControlShadowScanLimit = 64;

/** A system message a feature owns: it names a `control_type` (call control, membership, …). */
bool IsSystemControlMessage(const ThreadMessage& message);

/**
 * Mint or reuse the e2e_public Direct used for control to `peer_identity`. When `prefer_thread_id`
 * is already that peer's e2e_public Direct (the thread a feature acts in), return it instead of
 * creating a second catalog row.
 */
Roe<std::string> ResolveOrCreateE2ePublicDirectThread(IThreadStore& store, const std::string& peer_identity,
                                                     const std::optional<std::string>& prefer_thread_id,
                                                     const std::string& contact_id, const std::string& dm_title);

/**
 * True when `thread` is an e2e_public Direct and another Direct with the same peer uses private
 * e2e — the public row is the control twin.
 */
bool HasPrivateE2eSibling(const Thread& thread, const std::vector<Thread>& all_threads);

/**
 * True when `thread` is a control twin with nothing to show (empty preview, no unread): hidden from
 * the sessions list, so control does not grow a second DM.
 */
bool IsControlShadowThread(const Thread& thread, const std::vector<Thread>& all_threads);

/**
 * True when every message is a control message and the page is complete (`messages.size() <
 * scan_limit`). An empty transcript counts; hitting the limit cannot prove older history is
 * control only.
 */
bool TranscriptIsOnlyControl(const std::vector<ThreadMessage>& messages, size_t scan_limit);

/**
 * Delete idle control twins: a private e2e sibling exists, the transcript is empty or control only
 * within the scan limit, and the id is not in `protect_thread_ids`. Returns how many rows went.
 */
Roe<size_t> PruneOrphanControlShadows(IThreadStore& store,
                                      const std::unordered_set<std::string>& protect_thread_ids = {});

} // namespace pbr
