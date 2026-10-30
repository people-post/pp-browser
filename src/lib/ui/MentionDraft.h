#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

/**
 * The composer's @ popup and the mention records of the draft being typed (G015). Pure logic: the controller feeds
 * it the draft text and cursor, and applies the edits it returns. All ranges are UTF-8 byte ranges, '@' included;
 * RmlUi reports the cursor in code points (see CursorToByte / ByteToCursor).
 *
 * lib/ui must not include lib/messaging, so the records come out as plain structs (DraftMention) and the caller
 * turns them into a MentionsPayload.
 */

/** A group member the popup can offer. `name` is the member's OWN name, never the local remark. */
struct MentionCandidate {
  std::string account_id;
  std::string name;
};

/** One popup row. `index` is what Pick() takes. */
struct MentionRow {
  int index = 0;
  std::string label; // shown; carries a short id after a duplicate name
  bool everyone = false;
};

/** A new draft text for the controller to put in the box, with the cursor (in code points). */
struct DraftEdit {
  std::string text;
  int cursor_chars = 0;
};

/** One mention of the draft: the inserted `text` ("@name") sits at [start, end) of the draft. */
struct DraftMention {
  std::string account_id; // empty for the @everyone record
  bool all = false;
  size_t start = 0;
  size_t end = 0;
  std::string text;
};

/** Code-point cursor (RmlUi) to UTF-8 byte offset; past the end clamps to the size. */
size_t CursorToByte(const std::string& text, int cursor_chars);
/** UTF-8 byte offset to code-point cursor. */
int ByteToCursor(const std::string& text, size_t byte_offset);

class MentionDraft {
public:
  /** Loads the members when the popup opens (so a roster change shows up the next time). */
  using Loader = std::function<std::vector<MentionCandidate>()>;

  /** A group chat: `everyone_name` is the localized "Everyone" (offered only when `owner`). */
  void Enable(Loader loader, bool owner, std::string everyone_name);
  /** Not a group chat (or none open): no popup, no records. */
  void Disable();
  /** The loader may refresh this (an ownership change since Enable). */
  void SetOwner(const bool owner) { owner_ = owner; }
  bool Enabled() const { return enabled_; }
  /** The draft was replaced wholesale (another chat's draft): drop the records and the popup. */
  void Reset();

  /**
   * The draft or the cursor may have changed. `committed_input` is true when a textinput event (a character
   * that is really in the box) arrived since the last call; only that can open the popup. `composing` (an IME
   * composition in progress) makes the call a no-op: nothing opens, no range moves. Returns an edit when the
   * draft must be rewritten (a mention deleted as a whole).
   */
  std::optional<DraftEdit> OnDraftChanged(const std::string& text, int cursor_chars, bool committed_input,
                                          bool composing = false);

  bool PopupOpen() const { return open_; }
  /** Open and nothing matches: the popup shows "no matching members". */
  bool PopupEmpty() const { return open_ && rows_.empty(); }
  const std::vector<MentionRow>& Rows() const { return rows_; }
  void ClosePopup();

  /** Insert "@name " for the row, record it. nullopt when the popup is closed or the row is unknown. */
  std::optional<DraftEdit> Pick(int row_index);

  /**
   * The records to send. `draft_text` is the box content, `sent_text` what goes out (trimmed; a quote may
   * follow). A record survives only when its slice of `sent_text` still equals what was inserted; at most
   * kMax records and one @everyone. None left: plain text.
   */
  std::vector<DraftMention> FinalizeForSend(const std::string& draft_text, const std::string& sent_text) const;

  const std::vector<DraftMention>& Mentions() const { return mentions_; }

private:
  void Refresh(const std::string& query);
  void OpenFor(const std::string& text, size_t at_byte, size_t cursor_byte);
  bool InsideMention(size_t at_byte) const;

  bool enabled_ = false;
  bool owner_ = false;
  std::string everyone_name_;
  Loader loader_;

  std::string last_text_;
  std::vector<DraftMention> mentions_;

  bool open_ = false;
  size_t at_byte_ = 0;     // the '@' of the query being typed
  size_t cursor_byte_ = 0; // end of the query
  std::vector<MentionCandidate> members_;
  std::vector<MentionRow> rows_;
  std::vector<int> row_member_; // row -> index in members_, -1 for the Everyone row
};

} // namespace pbr
