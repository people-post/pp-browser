#pragma once

#include "common/thread/ThreadTypes.h"
#include "lib/calls/CallControlFamily.h"

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
 * How a call record looks, in the transcript and in the chat list (Kenneth 2026-11-13): the icon carries the
 * meaning (direction, missed, declined), the words stay short. `icon` is a file stem under `assets/icons/v2/`:
 * phone-outgoing | phone-incoming | phone-x (missed, declined and did-not-connect share the x), and for a video call
 * the camera twins video-outgoing | video-incoming | video-x. `text` is "1:05" or one short word; a group call
 * leads with "Group call".
 */
struct CallRowLook {
  std::string icon;
  std::string text;
};
CallRowLook CallRowLookOf(const CallSummaryRow& row);

/** A call summary's chat-list preview: its short words and the state icon (empty when the record does not decode). */
struct CallPreview {
  std::string text;
  std::string icon;
};

/**
 * A call summary row's preview for the chat list, in the reader's language: the same icon and short words the
 * transcript draws, or the call's title (no icon) when the record does not decode. None: not a System row holding
 * the stand-in text — a person may type "Voice call", and that stays their message.
 */
std::optional<CallPreview> StoredCallPreview(const ThreadMessage& latest);

} // namespace pbr
