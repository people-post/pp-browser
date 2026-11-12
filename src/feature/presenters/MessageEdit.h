#pragma once

#include "feature/presenters/MessageResend.h"
#include "feature/presenters/SendMark.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace pbr {

/**
 * Editing a sent message, in the chat screen (DEV/PLAN-message-edit.md): the composer's edit state per chat, and the
 * send mark an edited message of ours shows while its edit has not gone out. In memory only.
 */

/** A message being edited in one chat. */
struct EditingMessage {
  std::string message_id;
  /** What the composer started with: the message's text, or only the reply of a quote reply. */
  std::string original;
  /** A quote reply's quote, as stored ("\n\n> …"), put back under the edited reply unchanged. */
  std::string quote_suffix;
  /** The composer's text before editing began: it comes back when editing ends. */
  std::string draft_before;
};

/**
 * The edit state of each chat. Leaving a chat does not end its edit: it waits there with what was typed (the composer
 * text is kept per chat by ThreadDrafts) and the draft from before. Only the user's own cancel discards typed words.
 */
class ChatEdits {
public:
  /**
   * Starts editing `message_id` (its text `message_text`) in `thread_id`, the composer holding `composer_text`; returns
   * the text the composer takes. None while that chat is already editing a message: its half-edited words and its draft
   * from before stay (one edit at a time; the user ends it first).
   */
  std::optional<std::string> Begin(const std::string& thread_id, const std::string& message_id,
                                   const std::string& message_text, const std::string& composer_text);

  /** The edit open in `thread_id`, if any. */
  const EditingMessage* In(const std::string& thread_id) const;

  /** What Send does with `composer_text`: nothing (unchanged), or send `text` (the whole new message text). */
  struct Submit {
    bool unchanged = false;
    std::string text;
  };
  Submit Prepare(const std::string& thread_id, const std::string& composer_text) const;

  /** Editing ends (sent, unchanged, or the user's ×): returns the draft from before, for the composer. */
  std::string End(const std::string& thread_id);

  /**
   * The message went away while being edited (deleted on another device): editing ends. The composer keeps what was
   * typed when there was no draft before; otherwise it gets that draft back (the words were for a message that is gone).
   */
  std::string EndTargetGone(const std::string& thread_id, const std::string& composer_text);

  /** The chat was closed or cleared. */
  void Forget(const std::string& thread_id) { edits_.erase(thread_id); }

private:
  std::unordered_map<std::string, EditingMessage> edits_;
};

/** The edit row whose send gives `message` its mark: our edited text's applied edit (its `edited_by`). */
std::optional<std::string> EditRowIdOf(const ThreadMessage& message);

/** The mark an edited message shows for its edit `edit_row` (a clock while it is being sent, red when it failed). */
SendMark EditSendMark(const ThreadMessage& edit_row, std::optional<int64_t> started_ms, int64_t now_ms);

/** As NoteMarkChange, for an edit's mark. */
void NoteEditMarkChange(std::optional<int64_t>& earliest, const ThreadMessage& edit_row,
                        std::optional<int64_t> started_ms, int64_t now_ms);

/**
 * A tap on the mark of an edited message: Nothing (no failed edit), StillTrying (still being sent), ReasonOnly with
 * ThisKind (a private chat, whose numbering cannot drop the unsent row — as for a message), or Resent: the decision to
 * send the edit again (ConversationsFacade::ResendEdit does it).
 */
SendMarkTap DecideEditResendTap(const std::optional<ThreadMessage>& edit_row, const Thread& thread,
                                std::optional<int64_t> started_ms, int64_t now_ms);

} // namespace pbr
