#pragma once

#include "common/Error.h"
#include "common/chat/MessagingLimits.h"
#include "lib/delivery/SendRelayOptions.h"
#include "common/thread/IThreadStore.h"
#include "common/thread/IThreadTranscript.h"
#include "common/thread/ThreadRecordTypes.h"

#include <cstddef>
#include <cstdint>
#include <functional>
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
/**
 * Room left under kMaxE2ePlaintextBytes beside an edit's ChatPayload: a group copy wraps it with the group id, roster
 * epoch and hash (GroupE2ePayloadCodec::EncodeCopyPlaintext), about a hundred bytes.
 */
inline constexpr size_t kEditPayloadHeadroomBytes = 1024;

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

/**
 * Whether an edit of `target_message_id` to `text` goes out in one payload: at most kMaxEditTextBytes, and its ChatPayload
 * as the send encodes it (ChatPayloadCodec::EncodeToRow: the text twice, top-level `text` and `value`) within
 * kMaxE2ePlaintextBytes less kEditPayloadHeadroomBytes. A typed message near the limit, or a long quote reply (its quote
 * is part of the text), fits as a message but not as an edit.
 */
bool EditFitsOnTheWire(const std::string& target_message_id, const std::string& text);

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
 * Drops the queued send of one of our edits that will not go out now — its message deleted, or a newer edit made
 * (MeshDeliveryOrchestrator::CancelPendingSend; DeliveryOutbox::Cancel: its queued sends, a group member's copies too).
 */
using CancelEditSendFn = std::function<void(const std::string& edit_id)>;

/**
 * Blanks every edit row aimed at `target_id` that still has text, except `keep_id` (the one applied, "" for none — the
 * message was deleted). An edit of ours among them is not sent any more: `cancel_send` drops its queued send first (so
 * words the user replaced or deleted never go out). Returns how many rows were blanked.
 */
size_t BlankEditRowsFor(IThreadTranscript& store, const std::string& thread_id, const std::string& target_id,
                        const std::string& keep_id = {}, const CancelEditSendFn& cancel_send = {});

/** BlankEditRowsFor on `rows`, a message's annotation rows already read (the read is the costly part). */
size_t BlankEditRows(IThreadTranscript& store, const std::string& thread_id, const std::vector<ThreadMessage>& rows,
                     const std::string& keep_id, const CancelEditSendFn& cancel_send);

/**
 * After edit `applied_id` was applied to a message that was `before` it: the edit `before` carried (its `edited_by`),
 * blanked as BlankEditRowsFor does (ours cancelled). Only the applied edit row of a message keeps its words, so this
 * one row is all there is to blank: a few row reads, not a read of every annotation row of the message. Returns whether
 * a row was blanked.
 */
bool BlankReplacedEdit(IThreadTranscript& store, const std::string& thread_id, const ThreadMessage& before,
                       const std::string& applied_id, const CancelEditSendFn& cancel_send);

/**
 * How our edit of `target_message_id` goes out: an `edit` annotation carrying `text`, with the id and send time fixed
 * before the send (`message_id`, `sent_at_ms`) and no preview change. The send itself is the ordinary one: numbered,
 * queued, retried with the same envelope, copied to our other devices (public chats and groups).
 */
SendRelayOptions EditSendOptions(const std::string& target_message_id, const std::string& text,
                                 const std::string& message_id, int64_t sent_at_ms);

/**
 * Our edit `edit_row` (as stored by the send) applied to our message `target` here at once: the row rewritten
 * (WithEdit) and the edit it replaces blanked (BlankReplacedEdit); an earlier edit of ours not sent yet is cancelled
 * (`cancel_send`). Not applied (the row was not rewritten: the message deleted meanwhile, or the write failed), the
 * edit itself is blanked and cancelled, so nothing of it goes out. Returns the rewritten row.
 */
Roe<ThreadMessage> ApplyOwnEdit(IThreadTranscript& store, const ThreadMessage& target, const ThreadMessage& edit_row,
                                const CancelEditSendFn& cancel_send);

/**
 * One edit's send as MeshDeliveryOrchestrator::SendEdit runs it (numbered, stored, queued): the edit's row as stored, or
 * why it did not go out.
 */
using SendEditFn =
    std::function<Roe<ThreadMessage>(const std::string& edit_id, const std::string& text, int64_t sent_at_ms)>;

/**
 * The edit's row after its send `sent`: what the send returned, or — the send failed after it stored the row — that row,
 * marked failed (`stored_unsent` set; the message then shows the red mark). The send's error when nothing was stored.
 */
Roe<ThreadMessage> EditRowAfterSend(IThreadStore& store, const std::string& thread_id, const std::string& edit_id,
                                    Roe<ThreadMessage> sent, bool& stored_unsent);

/** What an edit of ours did here. */
struct OwnEdit {
  /** The message as it is now. */
  ThreadMessage row;
  /** The text was unchanged: nothing was sent. */
  bool unchanged = false;
  /** The edit's row was stored but its send failed: the message shows the red mark. */
  bool stored_unsent = false;
};

/**
 * ConversationsHub::EditMessage's steps for our message `target` (with its `thread_id`, in a chat of `thread_kind`) to
 * `new_text`, as tapped at `tapped_at_ms`: checked — CanEditMessage at the tap, the text not empty and
 * EditFitsOnTheWire — and refused before anything is stored or sent; unchanged text sends nothing; otherwise sent as
 * `edit_id` (`send`, stamped EditSendTime) and applied here (ApplyOwnEdit: the edit it replaces, if not sent yet, is
 * cancelled with `cancel_send`).
 */
Roe<OwnEdit> EditOwnMessage(IThreadStore& store, const ThreadMessage& target, ThreadKind thread_kind,
                            const std::string& new_text, int64_t tapped_at_ms, const std::string& edit_id,
                            const SendEditFn& send, const CancelEditSendFn& cancel_send);

/** What a resend of our edit did. */
struct ResentEdit {
  /** The new edit's row. */
  ThreadMessage edit_row;
  /** It was stored but its send failed again. */
  bool stored_unsent = false;
  /** The unsent edit's row was taken away. */
  bool unsent_removed = false;
};

/**
 * ConversationsHub::ResendEdit's steps (the red mark on our edited message `message_id`, tapped): its edit that did not
 * go out (the one it carries, `edited_by`) is sent again as `edit_id` (`send`) with the same words and the same send
 * time, applied here (the message's `edited_by` becomes the new one, every other edit row blanked), and the unsent one
 * goes (ApplyOwnEdit drops its queued send with `cancel_pending`, then its row is removed). Refused, nothing sent, when
 * the message is not an edited text of ours or its edit is not there, blank, or already went out (Relayed).
 */
Roe<ResentEdit> ResendOwnEdit(IThreadStore& store, const std::string& thread_id, const std::string& message_id,
                              const std::string& edit_id, const SendEditFn& send,
                              const std::function<void(const std::string& message_id)>& cancel_pending);

} // namespace pbr
