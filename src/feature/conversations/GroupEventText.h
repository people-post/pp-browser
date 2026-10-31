#pragma once

#include "common/thread/ThreadTypes.h"

#include <functional>
#include <optional>
#include <string>

#include "common/PbrCompat.h"

namespace pbr {

/**
 * A group thread's membership line in the reader's language with the person's public name (joined, removed, left,
 * renamed, declined, ownership, group full, forked). The stored text is English and names nobody; it stays for
 * old rows and for kinds with no wording here. `member_name`: an account's public name (never a local remark);
 * `local_account`: this account, whose lines read "you". Not escaped: the caller escapes what it draws.
 * None: not a membership line, or one that does not decode — show the stored text.
 */
std::optional<std::string> GroupEventText(const ThreadMessage& message,
                                          const std::function<std::string(const std::string& account_id)>& member_name,
                                          const std::string& local_account);

/** Whether `preview` (a thread's stored preview) is the stored English text of a membership line. */
bool IsStoredGroupEventText(const std::string& preview);

} // namespace pbr
