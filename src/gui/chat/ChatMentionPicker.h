#pragma once

#include "feature/conversations/ChatPresenter.h"
#include "feature/conversations/GroupMentions.h"
#include "gui/chat/ChatMenus.h"
#include "gui/chat/ChatScreenState.h"

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace ui {
class Context;
}

namespace pbr {

/**
 * The composer's `@` picker in a group chat (group-chat G015). Typing `@` at the start or after a space opens
 * a menu of the group's other members by public nickname (`@everyone` too for the owner); typing on filters
 * it, a finished word or Escape closes it. The query runs from the `@` to the caret, so an `@` typed in front of a
 * word leaves the word alone. Choosing puts `@<nickname> ` in the draft and records the mention;
 * at send, SpansFor() finds the recorded tokens in the final text. Recorded mentions are kept per chat, like
 * its draft. The lists and the span rules are GroupMentions' (feature layer).
 *
 * UI thread.
 */
class ChatMentionPicker {
public:
  ChatMentionPicker(ChatMenusHost& host, ChatPresenter& presenter, ChatScreenState& chat, ui::Context*& context);

  /**
   * The open chat or its group may have changed (a chat opened, messaging refreshed): keeps the group's lines
   * ready — read once on a worker when a group chat opens, again when the group changes, dropped on another chat.
   */
  void SyncThread();
  /** The composer's text changed (typing, or the app setting it): opens, filters or closes the picker. */
  void OnDraftChanged();
  bool IsOpen() const;
  void Close();
  /** Enter / Tab while open: choose the first line. False when nothing was chosen. */
  bool ChooseFirst();
  /** The spans to send `text` (the draft as sent, before any quote) with in `thread_id`. */
  std::vector<TextSpan> SpansFor(const std::string& thread_id, const std::string& text) const;
  /** The chat's draft went out (or was dropped): its recorded mentions go too. */
  void ForgetThread(const std::string& thread_id);
  /** A new profile: nothing recorded for the old one stays. */
  void Reset();

private:
  /** The draft's text as the input holds it now. */
  std::string CurrentDraft() const;
  /** The caret in `text`, as a byte offset (the input counts code points). */
  size_t CaretByte(const std::string& text) const;
  /** Query and show the lines for the draft as it is now (the picker is open on `at_`). */
  void RefreshLines();
  void Show(const std::vector<MentionCandidate>& lines);
  void Choose(const MentionCandidate& line);

  ChatMenusHost& host_;
  ChatPresenter& presenter_;
  ChatScreenState& chat_;
  ui::Context*& context_;
  /** The draft at the last change, and whose it was (a switch of chats is not an edit). */
  std::string last_text_;
  std::string last_thread_;
  /** While open: the byte offset of its `@`, the caret when the lines were made, and the lines shown. */
  std::optional<size_t> at_;
  size_t caret_ = 0;
  std::vector<MentionCandidate> lines_;
  /** All the open group's lines, read ahead on a worker; the picker filters them. */
  MentionCandidatePreload preload_;
  /** `@` typed before the lines were there: the picker opens when they arrive. */
  bool waiting_ = false;
  std::map<std::string, std::vector<ComposerMention>> recorded_;
};

} // namespace pbr
