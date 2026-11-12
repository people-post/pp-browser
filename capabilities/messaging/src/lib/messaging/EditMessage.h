#pragma once

#include "common/chat/MessagingLimits.h"
#include "common/thread/IThreadTranscript.h"
#include "common/thread/ThreadRecordTypes.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

/**
 * Editing a sent message (DEV/PLAN-message-edit.md, WIRE_SCHEMAS.md `annotation_type = "edit"`). An edit is an
 * annotation from the message's own sender whose `value` is the whole new text; where it is accepted the target row is
 * rewritten in place (same id, seq, order and send time) and marked in its `payload_json` with `edited_at` (the edit's
 * send time) and `edited_by` (the edit's message id). Only the newest accepted edit's row keeps its text; every other
 * edit row aimed at the same message is blanked (kept for its id and seq, its text gone).
 */

/** Our own message can be edited this long after it was sent. */
inline constexpr int64_t kEditWindowMs = 15 * 60'000;
/** A receiver accepts an edit sent up to this much later still (clocks, a send queued at the last moment). */
inline constexpr int64_t kEditArrivalSlackMs = 60'000;
/** The new text's limit: a typed message's. */
inline constexpr size_t kMaxEditTextBytes = kMaxComposeTextBytes;

/** `payload_json` keys of an edited message row (DATA_LAYOUT.md). */
inline constexpr const char* kEditedAtKey = "edited_at";
inline constexpr const char* kEditedByKey = "edited_by";

/**
 * The menu's and the send's rule: our own text message, out on the wire (delivered, or sent by another of our devices),
 * not a local / app-drawn row, not an AI question, not in an AI conversation, sent at most kEditWindowMs before
 * `now_ms`.
 */
bool CanEditMessage(const ThreadMessage& message, ThreadKind thread_kind, int64_t now_ms);

/** The edit annotation's JSON (any text: escaped as JSON, unlike the emoji-only reaction builder). */
std::string BuildEditPayloadJson(const std::string& target_message_id, const std::string& text);

/** An annotation of type edit (blanked or not). */
bool IsEditAnnotation(const ThreadMessage& message);

/** The new text an edit annotation carries ("" for a blanked one); none for anything else. */
std::optional<std::string> EditTextOf(const ThreadMessage& annotation);

/** The mark of an edited message row. */
struct EditMark {
  int64_t edited_at = 0;
  std::string edited_by;
};

/** The edit mark in `message.payload_json`; none when the message was never edited. */
std::optional<EditMark> EditMarkOf(const ThreadMessage& message);

/** Why an edit is not applied; None when it is. */
enum class EditRefusal {
  None,
  /** Not an edit annotation aimed at this message. */
  NotEdit,
  /** 1. Not from the message's own sender. */
  NotFromAuthor,
  /** 2. The message is deleted. */
  TargetDeleted,
  /** 2. The message is not a text message. */
  NotText,
  /** 3. No new text (also every blanked edit row). */
  EmptyText,
  /** 3. Longer than kMaxEditTextBytes. */
  TooLong,
  /** 4. Sent more than kEditWindowMs + kEditArrivalSlackMs after the message. */
  TooLate,
  /** 5. Not newer than the edit the message already carries. */
  NotNewer,
};

/** A short name for the log (no message content). */
const char* EditRefusalName(EditRefusal refusal);

/**
 * The receiver's five conditions for `edit` on `target`, both as stored here (sender = the verified sender, timestamp =
 * the sender's send time from the envelope). Newer (5) is by the edit's own send time, then by the larger message id.
 */
EditRefusal CheckEdit(const ThreadMessage& edit, const ThreadMessage& target);

/** `target` with `edit` applied: the new text, plain (mention / formatting spans dropped), and the edit mark. */
ThreadMessage WithEdit(ThreadMessage target, const ThreadMessage& edit);

/** `edit` with its text taken out: the row stays (its id, seq, target and time), inert (EmptyText). */
ThreadMessage BlankedEdit(ThreadMessage edit);

/**
 * The send time of a new edit of `target`: `now_ms`, but never before the edit this device already applied to it (+1 ms),
 * so an edit made after receiving our other device's edit wins whatever this device's clock says.
 */
int64_t EditSendTime(int64_t now_ms, const ThreadMessage& target);

/**
 * When `target` arrives with edits already stored for it (an edit can overtake its message): the newest of them that
 * passes CheckEdit against it, none when none does.
 */
std::optional<ThreadMessage> NewestPassingEdit(const std::vector<ThreadMessage>& annotations,
                                               const ThreadMessage& target);

/**
 * Blanks every edit row aimed at `target_id` that still has text, except `keep_id` (the one applied, "" for none — the
 * message was deleted). Returns how many rows were blanked.
 */
size_t BlankEditRowsFor(IThreadTranscript& store, const std::string& thread_id, const std::string& target_id,
                        const std::string& keep_id = {});

} // namespace pbr
