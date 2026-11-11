#pragma once

#include "common/thread/ThreadTypes.h"
#include "feature/conversations/InboxController.h"
#include "lib/delivery/DeliveryNotice.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace pbr {

/**
 * The mark beside a message we sent (#210, DEV/PLAN-message-send-status.md): a clock while a send attempt of it is
 * under way and started at most kQueuedMarkLimitMs ago; the red mark when it did not go out — retries used up, the
 * clock's time over, failed before it was sent, a restart since (nothing is under way then), a group message no
 * member got; nothing once it is out. Messages of others, AI / local rows, system lines and anything that never goes
 * on the wire carry none, and so does a group message that had nobody to go to (we were alone in the group): the red
 * mark means it was meant for someone and did not reach them.
 */

/** A clock shows at most this long after the send started; past it, the red mark (retries may still run). */
inline constexpr int64_t kQueuedMarkLimitMs = 60'000;
/** A new row whose send is not registered yet shows the clock this long, so it does not flash red. */
inline constexpr int64_t kQueuedMarkGraceMs = 5'000;

/** `message` can carry a send mark at all: ours, sent to people, a message bubble (text, picture, file, voice, card). */
bool MayShowSendMark(const ThreadMessage& message);

/**
 * The mark on `message` at `now_ms`. `started_ms`: when its send still under way started (DeliveryOutbox), none when
 * nothing of it is under way. `no_recipients`: a group message that had nobody to go to (GroupRowHadRecipients).
 */
SendMark MarkOf(const ThreadMessage& message, std::optional<int64_t> started_ms, int64_t now_ms,
                bool no_recipients = false);

/**
 * Folds `message` into `earliest`, the soonest time a mark shown at `now_ms` changes on its own (a clock turning red):
 * `earliest` keeps the sooner of the two. The chat redraws then.
 */
void NoteMarkChange(std::optional<int64_t>& earliest, const ThreadMessage& message, std::optional<int64_t> started_ms,
                    int64_t now_ms, bool no_recipients = false);

/**
 * Whether a group message sent at `sent_at` had anyone to go to: some other member now on the roster had joined by then
 * (`other_members_joined_at`, their roster join times; 0 = not known, counted as there). Nothing about the send is
 * stored, so this is read from the roster as it is now: someone who has left since no longer counts (the message then
 * shows no mark), and a join time that was rewritten later (a roster rebuild) hides a mark the same way; a message
 * written while alone stays unmarked after others join.
 */
bool GroupRowHadRecipients(const std::vector<int64_t>& other_members_joined_at, int64_t sent_at);

/**
 * Why our message did not go out, for the pill a tap on its red mark shows: the reason of its stored "did not go out"
 * line (`failure_row`, the row SendFailureRowId(message id, "") names) in a 1:1 chat. None — the generic sentence —
 * when there is no such line (failed before it was sent, a restart, a group), or its reason is not known.
 */
std::optional<SendFailure> SendFailureReason(const std::optional<ThreadMessage>& failure_row, bool group_thread);

} // namespace pbr
