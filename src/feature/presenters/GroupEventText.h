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

/**
 * The words of a system line its writer stores in English (a joined / declined invitation, a blocked inviter, the
 * owner-unreachable note, the sender's own rotate notice), in the reader's language; the stored row is not
 * rewritten. None: not such a line — show the stored text. Only for a System row: the same words typed by a person
 * are their message. (The stored text is also the chat's preview and a notice's text.)
 */
std::optional<std::string> StoredSystemLineText(const std::string& stored);

/** Whether `preview` (a thread's stored preview) is the stored English text of a membership line. */
bool IsStoredGroupEventText(const std::string& preview);

/** Whether `preview` (a thread's stored preview) is the English stand-in text of a call summary. */
bool IsStoredCallPreview(const std::string& preview);

/**
 * A call summary row's words for the chat list's preview, in the reader's language: the line the transcript's family
 * draws (missed, declined, duration …), or the call's title when the record does not decode. None: not a System row
 * holding the stand-in text — a person may type "Voice call", and that stays their message.
 */
std::optional<std::string> StoredCallPreviewText(const ThreadMessage& latest);

} // namespace pbr
