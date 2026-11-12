#include "lib/messaging/EditMessage.h"

#include "lib/delivery/ChatPayloadCodec.h"
#include "lib/delivery/ReactionTypes.h"

#include "common/ValueJson.h"

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

size_t BlankEditRowsFor(IThreadTranscript& store, const std::string& thread_id, const std::string& target_id,
                        const std::string& keep_id) {
  auto annotations = store.GetAnnotationsForTarget(thread_id, target_id);
  if (!annotations) {
    return 0;
  }
  size_t blanked = 0;
  for (const ThreadMessage& annotation : *annotations) {
    if (annotation.id == keep_id) {
      continue;
    }
    const std::optional<std::string> text = EditTextOf(annotation);
    if (!text || (text->empty() && annotation.text.empty())) {
      continue;
    }
    ThreadMessage blank = BlankedEdit(annotation);
    blank.thread_id = thread_id;
    if (auto updated = store.UpdateMessage(blank); updated && *updated) {
      ++blanked;
    }
  }
  return blanked;
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

Roe<ThreadMessage> ApplyOwnEdit(IThreadTranscript& store, const ThreadMessage& target, const ThreadMessage& edit_row) {
  ThreadMessage edited = WithEdit(target, edit_row);
  edited.thread_id = target.thread_id;
  auto updated = store.UpdateMessage(edited);
  if (!updated) {
    return updated.error();
  }
  if (!*updated) {
    return Error("The message to edit is gone");
  }
  (void)BlankEditRowsFor(store, target.thread_id, target.id, edit_row.id);
  return edited;
}

} // namespace pbr
