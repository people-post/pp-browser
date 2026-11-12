#include "lib/messaging/EditMessage.h"

#include "lib/delivery/ChatPayloadCodec.h"
#include "lib/delivery/ReactionTypes.h"

#include "common/ValueJson.h"
#include "common/chat/MessagingLimits.h"

#include <algorithm>

namespace pbr {

bool CanEditMessage(const ThreadMessage& message, const ThreadKind thread_kind, const int64_t now_ms) {
  if (thread_kind == ThreadKind::Ai || message.sender_contact_id != kLocalSelfContactId) {
    return false;
  }
  if (message.content_type != ChatContentType::Text || message.content_rml || !message.relay_visible ||
      IsLocalActionMessage(message) || message.ai_invoke_mode) {
    return false;
  }
  // Out on the wire: a queued or failed message is not edited (it is sent again instead).
  if (message.delivery != MessageDelivery::Relayed) {
    return false;
  }
  return now_ms - message.timestamp <= kEditWindowMs;
}

std::string BuildEditPayloadJson(const std::string& target_message_id, const std::string& text) {
  Object payload;
  payload.set("annotation_type", kAnnotationTypeEdit);
  payload.set("target_message_id", target_message_id);
  payload.set("value", text);
  return DumpJson(payload);
}

bool EditFitsOnTheWire(const std::string& target_message_id, const std::string& text) {
  if (text.size() > kMaxEditTextBytes) {
    return false;
  }
  ThreadMessage edit; // as the send puts it on the wire (EditSendOptions)
  edit.content_type = ChatContentType::Annotation;
  edit.text = text;
  edit.target_message_id = target_message_id;
  edit.payload_json = BuildEditPayloadJson(target_message_id, text);
  auto row = ChatPayloadCodec::EncodeToRow(edit);
  return row && row->size() + kEditPayloadHeadroomBytes <= kMaxE2ePlaintextBytes;
}

bool IsEditAnnotation(const ThreadMessage& message) {
  if (message.content_type != ChatContentType::Annotation) {
    return false;
  }
  auto fields = ChatPayloadCodec::DecodeAnnotationJson(message.payload_json);
  return fields && fields->annotation_type == kAnnotationTypeEdit;
}

std::optional<std::string> EditTextOf(const ThreadMessage& annotation) {
  if (annotation.content_type != ChatContentType::Annotation) {
    return std::nullopt;
  }
  auto fields = ChatPayloadCodec::DecodeAnnotationJson(annotation.payload_json);
  if (!fields || fields->annotation_type != kAnnotationTypeEdit) {
    return std::nullopt;
  }
  return fields->value;
}

std::optional<EditMark> EditMarkOf(const ThreadMessage& message) {
  if (message.payload_json.empty()) {
    return std::nullopt;
  }
  auto payload = TryParseObject(message.payload_json);
  if (!payload) {
    return std::nullopt;
  }
  auto at = payload->getIf<int64_t>(kEditedAtKey);
  auto by = payload->getString(kEditedByKey);
  if (!at || !by || by->empty()) {
    return std::nullopt;
  }
  return EditMark{*at, *by};
}

const char* EditRefusalName(const EditRefusal refusal) {
  switch (refusal) {
    case EditRefusal::None:
      return "none";
    case EditRefusal::NotEdit:
      return "not_edit";
    case EditRefusal::NotFromAuthor:
      return "not_from_author";
    case EditRefusal::TargetDeleted:
      return "target_deleted";
    case EditRefusal::NotText:
      return "not_text";
    case EditRefusal::EmptyText:
      return "empty_text";
    case EditRefusal::TooLong:
      return "too_long";
    case EditRefusal::TooLate:
      return "too_late";
    case EditRefusal::NotNewer:
      return "not_newer";
  }
  return "unknown";
}

EditRefusal CheckEdit(const ThreadMessage& edit, const ThreadMessage& target) {
  const std::optional<std::string> text = EditTextOf(edit);
  if (!text || edit.target_message_id != target.id) {
    return EditRefusal::NotEdit;
  }
  if (edit.sender_contact_id != target.sender_contact_id) {
    return EditRefusal::NotFromAuthor;
  }
  if (target.content_type == ChatContentType::Deleted) {
    return EditRefusal::TargetDeleted;
  }
  if (target.content_type != ChatContentType::Text || target.content_rml) {
    return EditRefusal::NotText;
  }
  if (text->empty()) {
    return EditRefusal::EmptyText;
  }
  if (text->size() > kMaxEditTextBytes) {
    return EditRefusal::TooLong;
  }
  if (edit.timestamp - target.timestamp > kEditWindowMs + kEditArrivalSlackMs) {
    return EditRefusal::TooLate;
  }
  if (const std::optional<EditMark> current = EditMarkOf(target)) {
    const bool newer = edit.timestamp > current->edited_at ||
                       (edit.timestamp == current->edited_at && edit.id > current->edited_by);
    if (!newer) {
      return EditRefusal::NotNewer;
    }
  }
  return EditRefusal::None;
}

ThreadMessage WithEdit(ThreadMessage target, const ThreadMessage& edit) {
  target.text = EditTextOf(edit).value_or(target.text);
  // Plain text from now on: the spans (mentions) pointed into the old words.
  Object payload;
  payload.set(kEditedAtKey, edit.timestamp);
  payload.set(kEditedByKey, edit.id);
  target.payload_json = DumpJson(payload);
  return target;
}

ThreadMessage BlankedEdit(ThreadMessage edit) {
  edit.text.clear();
  edit.payload_json = BuildEditPayloadJson(edit.target_message_id.value_or(""), "");
  return edit;
}

int64_t EditSendTime(const int64_t now_ms, const ThreadMessage& target) {
  if (const std::optional<EditMark> current = EditMarkOf(target)) {
    return std::max(now_ms, current->edited_at + 1);
  }
  return now_ms;
}

std::optional<ThreadMessage> NewestPassingEdit(const std::vector<ThreadMessage>& annotations,
                                               const ThreadMessage& target) {
  std::optional<ThreadMessage> newest;
  for (const ThreadMessage& annotation : annotations) {
    if (!IsEditAnnotation(annotation)) {
      continue;
    }
    // Each against the message as it arrived, plus the newest so far (condition 5).
    const ThreadMessage base = newest ? WithEdit(target, *newest) : target;
    if (CheckEdit(annotation, base) == EditRefusal::None) {
      newest = annotation;
    }
  }
  return newest;
}

namespace {

/**
 * Stored edit row `edit` (with text) blanked. Ours is not sent any more: its queued send is dropped first, whatever
 * its row says, as a group member's copy can still be queued after another member's went out (a no-op when none is).
 */
bool BlankEditRow(IThreadTranscript& store, const std::string& thread_id, const ThreadMessage& edit,
                  const CancelEditSendFn& cancel_send) {
  if (cancel_send && edit.sender_contact_id == kLocalSelfContactId) {
    cancel_send(edit.id);
  }
  ThreadMessage blank = BlankedEdit(edit);
  blank.thread_id = thread_id;
  auto updated = store.UpdateMessage(blank);
  return updated && *updated;
}

} // namespace

size_t BlankEditRowsFor(IThreadTranscript& store, const std::string& thread_id, const std::string& target_id,
                        const std::string& keep_id, const CancelEditSendFn& cancel_send) {
  auto annotations = store.GetAnnotationsForTarget(thread_id, target_id);
  if (!annotations) {
    return 0;
  }
  return BlankEditRows(store, thread_id, *annotations, keep_id, cancel_send);
}

size_t BlankEditRows(IThreadTranscript& store, const std::string& thread_id, const std::vector<ThreadMessage>& rows,
                     const std::string& keep_id, const CancelEditSendFn& cancel_send) {
  size_t blanked = 0;
  for (const ThreadMessage& annotation : rows) {
    if (annotation.id == keep_id) {
      continue;
    }
    const std::optional<std::string> text = EditTextOf(annotation);
    if (!text || (text->empty() && annotation.text.empty())) {
      continue;
    }
    if (BlankEditRow(store, thread_id, annotation, cancel_send)) {
      ++blanked;
    }
  }
  return blanked;
}

bool BlankReplacedEdit(IThreadTranscript& store, const std::string& thread_id, const ThreadMessage& before,
                       const std::string& applied_id, const CancelEditSendFn& cancel_send) {
  const std::optional<EditMark> replaced = EditMarkOf(before);
  if (!replaced || replaced->edited_by == applied_id) {
    return false;
  }
  auto found = store.GetMessage(thread_id, replaced->edited_by);
  if (!found || !found->has_value() || (*found)->target_message_id != before.id ||
      EditTextOf(**found).value_or("").empty()) {
    return false;
  }
  return BlankEditRow(store, thread_id, **found, cancel_send);
}

SendRelayOptions EditSendOptions(const std::string& target_message_id, const std::string& text,
                                 const std::string& message_id, const int64_t sent_at_ms) {
  SendRelayOptions options;
  options.content_type = ChatContentType::Annotation;
  options.payload_json = BuildEditPayloadJson(target_message_id, text);
  options.update_preview = false;
  options.message_id = message_id;
  options.timestamp_ms = sent_at_ms;
  return options;
}

Roe<ThreadMessage> ApplyOwnEdit(IThreadTranscript& store, const ThreadMessage& target, const ThreadMessage& edit_row,
                                const CancelEditSendFn& cancel_send) {
  ThreadMessage edited = WithEdit(target, edit_row);
  edited.thread_id = target.thread_id;
  auto updated = store.UpdateMessage(edited);
  if (!updated || !*updated) {
    (void)BlankEditRow(store, target.thread_id, edit_row, cancel_send); // not applied here: not sent either
    if (!updated) {
      return updated.error();
    }
    return Error("The message to edit is gone");
  }
  (void)BlankReplacedEdit(store, target.thread_id, target, edit_row.id, cancel_send);
  return edited;
}

Roe<ThreadMessage> EditRowAfterSend(IThreadStore& store, const std::string& thread_id, const std::string& edit_id,
                                    Roe<ThreadMessage> sent, bool& stored_unsent) {
  if (sent) {
    return sent;
  }
  auto stored = store.GetMessage(thread_id, edit_id);
  if (!stored || !*stored) {
    return sent.error(); // refused before anything was stored: nothing changes here
  }
  stored_unsent = true;
  return **stored;
}

Roe<OwnEdit> EditOwnMessage(IThreadStore& store, const ThreadMessage& target, const ThreadKind thread_kind,
                            const std::string& new_text, const int64_t tapped_at_ms, const std::string& edit_id,
                            const SendEditFn& send, const CancelEditSendFn& cancel_send) {
  if (!CanEditMessage(target, thread_kind, tapped_at_ms)) {
    return Error("This message can no longer be edited");
  }
  if (new_text.empty() || !EditFitsOnTheWire(target.id, new_text)) {
    return Error("Edited text is empty or too long");
  }
  OwnEdit result;
  if (new_text == target.text) {
    result.row = target;
    result.unchanged = true;
    return result;
  }
  auto edit_row = EditRowAfterSend(store, target.thread_id, edit_id,
                                   send(edit_id, new_text, EditSendTime(tapped_at_ms, target)), result.stored_unsent);
  if (!edit_row) {
    return edit_row.error();
  }
  auto edited = ApplyOwnEdit(store, target, *edit_row, cancel_send);
  if (!edited) {
    return edited.error();
  }
  result.row = std::move(*edited);
  return result;
}

Roe<ResentEdit> ResendOwnEdit(IThreadStore& store, const std::string& thread_id, const std::string& message_id,
                              const std::string& edit_id, const SendEditFn& send,
                              const std::function<void(const std::string&)>& cancel_pending) {
  auto found = store.GetMessage(thread_id, message_id);
  if (!found || !found->has_value()) {
    return Error("Message not found");
  }
  const ThreadMessage target = **found;
  const std::optional<EditMark> mark = EditMarkOf(target);
  if (target.sender_contact_id != kLocalSelfContactId || target.content_type != ChatContentType::Text || !mark) {
    return Error("Not an edited message of ours");
  }
  auto unsent = store.GetMessage(thread_id, mark->edited_by);
  if (!unsent || !unsent->has_value()) {
    return Error("Its edit is gone");
  }
  const ThreadMessage old_edit = **unsent;
  const std::optional<std::string> text = EditTextOf(old_edit);
  if (!text || text->empty() || old_edit.delivery == MessageDelivery::Relayed) {
    return Error("Nothing to send again");
  }
  // Same words, same send time (the time its receivers judge); a new id, as a re-sent message has.
  ResentEdit result;
  auto edit_row =
      EditRowAfterSend(store, thread_id, edit_id, send(edit_id, *text, old_edit.timestamp), result.stored_unsent);
  if (!edit_row) {
    return edit_row.error();
  }
  if (auto edited = ApplyOwnEdit(store, target, *edit_row, cancel_pending); !edited) {
    return edited.error();
  }
  auto removed = store.RemoveUnsentMessage(thread_id, old_edit.id); // its queued send dropped by ApplyOwnEdit
  result.unsent_removed = removed && *removed;
  result.edit_row = std::move(*edit_row);
  return result;
}

} // namespace pbr
