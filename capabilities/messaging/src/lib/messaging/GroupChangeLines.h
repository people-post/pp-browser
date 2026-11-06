#pragma once

#include "lib/messaging/GroupChange.h"
#include "common/thread/IThreadStore.h"
#include "common/thread/ThreadTypes.h"

#include "common/Error.h"

#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The group chat's line for a roster change that now counts (G018): stored as the membership line it reads as — a
 * leave as `member_left` ("Bob left the group"), a removal as `member_removed` with who removed them ("Bob removed
 * Carol"), an add as `member_joined` — so the transcript words it as it words the owner's events. Renames and links
 * have no line yet (o4, o5).
 */
namespace group_change_lines {

/** The line for `change` in the thread `thread_id`, at the roster epoch `epoch`; none for a kind with no line. */
std::optional<ThreadMessage> LineFor(const std::string& thread_id, const GroupChange& change, uint64_t epoch);

/**
 * Append each change's line to its group's chat (when the chat is there). Returns the thread when a line was added
 * about `local_identity` being removed (the caller notices it: the group goes read-only), else none.
 */
Roe<std::optional<std::string>> Append(IThreadStore& store, const std::string& group_id,
                                       const std::vector<GroupChange>& changes, uint64_t epoch,
                                       const std::string& local_identity);

} // namespace group_change_lines

} // namespace pbr
