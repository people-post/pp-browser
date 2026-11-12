#include "feature/presenters/MessageEdit.h"

#include "common/Utilities.h"
#include "lib/messaging/EditMessage.h"

namespace pbr {

std::optional<std::string> ChatEdits::Begin(const std::string& thread_id, const std::string& message_id,
                                            const std::string& message_text, const std::string& composer_text) {
  if (edits_.count(thread_id) != 0) {
    return std::nullopt; // already editing here: nothing typed is overwritten
  }
  EditingMessage editing;
  editing.message_id = message_id;
  editing.original = message_text;
  // A quote reply: only the reply is edited; the quote under it stays as it was sent.
  if (const size_t split = message_text.find("\n\n> "); split != std::string::npos && split > 0) {
    editing.original = message_text.substr(0, split);
    editing.quote_suffix = message_text.substr(split);
  }
  editing.draft_before = composer_text;
  const std::string composer = editing.original;
  edits_[thread_id] = std::move(editing);
  return composer;
}

const EditingMessage* ChatEdits::In(const std::string& thread_id) const {
  const auto found = edits_.find(thread_id);
  return found == edits_.end() ? nullptr : &found->second;
}

ChatEdits::Submit ChatEdits::Prepare(const std::string& thread_id, const std::string& composer_text) const {
  Submit submit;
  const EditingMessage* editing = In(thread_id);
  if (!editing) {
    return submit;
  }
  const std::string typed = util::Trim(composer_text);
  if (typed == util::Trim(editing->original)) {
    submit.unchanged = true;
    return submit;
  }
  submit.text = typed + editing->quote_suffix;
  return submit;
}

std::string ChatEdits::End(const std::string& thread_id) {
  const auto found = edits_.find(thread_id);
  if (found == edits_.end()) {
    return {};
  }
  std::string draft = std::move(found->second.draft_before);
  edits_.erase(found);
  return draft;
}

std::string ChatEdits::EndTargetGone(const std::string& thread_id, const std::string& composer_text) {
  const std::string draft = End(thread_id);
  return draft.empty() ? composer_text : draft;
}

std::optional<std::string> EditRowIdOf(const ThreadMessage& message) {
  if (message.sender_contact_id != kLocalSelfContactId || message.content_type != ChatContentType::Text) {
    return std::nullopt;
  }
  const std::optional<EditMark> mark = EditMarkOf(message);
  return mark ? std::optional<std::string>(mark->edited_by) : std::nullopt;
}

namespace {

/** The edit row read as the message it changes, so the message rules for marks apply to it. */
ThreadMessage AsMarkedMessage(ThreadMessage edit_row) {
  edit_row.content_type = ChatContentType::Text;
  edit_row.content_rml.reset();
  return edit_row;
}

} // namespace

SendMark EditSendMark(const ThreadMessage& edit_row, const std::optional<int64_t> started_ms, const int64_t now_ms) {
  if (!IsEditAnnotation(edit_row)) {
    return SendMark::None;
  }
  return MarkOf(AsMarkedMessage(edit_row), started_ms, now_ms);
}

void NoteEditMarkChange(std::optional<int64_t>& earliest, const ThreadMessage& edit_row,
                        const std::optional<int64_t> started_ms, const int64_t now_ms) {
  if (IsEditAnnotation(edit_row)) {
    NoteMarkChange(earliest, AsMarkedMessage(edit_row), started_ms, now_ms);
  }
}

SendMarkTap DecideEditResendTap(const std::optional<ThreadMessage>& edit_row, const Thread& thread,
                                const std::optional<int64_t> started_ms, const int64_t now_ms) {
  SendMarkTap tap;
  if (!edit_row || !IsEditAnnotation(*edit_row) || edit_row->sender_contact_id != kLocalSelfContactId) {
    return tap;
  }
  if (started_ms) {
    tap.kind = SendMarkTap::Kind::StillTrying;
    return tap;
  }
  if (EditSendMark(*edit_row, std::nullopt, now_ms) != SendMark::Failed) {
    return tap;
  }
  if (thread.kind == ThreadKind::Direct && thread.channel == ThreadChannel::E2e) {
    tap.kind = SendMarkTap::Kind::ReasonOnly;
    tap.refusal = SendMarkTap::Refusal::ThisKind;
    return tap;
  }
  if (thread.kind != ThreadKind::Direct && thread.kind != ThreadKind::Group) {
    return tap;
  }
  tap.kind = SendMarkTap::Kind::Resent;
  return tap;
}

} // namespace pbr
