#include "lib/ui/MentionDraft.h"

#include "common/directory/ShortRelayId.h"

#include <algorithm>
#include <cctype>
#include <map>

namespace pbr {

namespace {

bool IsContinuation(const unsigned char byte) {
  return (byte & 0xC0) == 0x80;
}

bool IsSpace(const char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

/** Steps `pos` back to the start of the UTF-8 character it sits in. */
size_t BackToBoundary(const std::string& s, size_t pos) {
  while (pos > 0 && pos < s.size() && IsContinuation(static_cast<unsigned char>(s[pos]))) {
    --pos;
  }
  return pos;
}

/** ASCII-only lowercase; other bytes compare exactly. */
std::string Fold(const std::string& s) {
  std::string out = s;
  for (char& c : out) {
    if (static_cast<unsigned char>(c) < 0x80) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
  }
  return out;
}

} // namespace

size_t CursorToByte(const std::string& text, const int cursor_chars) {
  if (cursor_chars <= 0) {
    return 0;
  }
  int chars = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    if (IsContinuation(static_cast<unsigned char>(text[i]))) {
      continue;
    }
    if (chars == cursor_chars) {
      return i;
    }
    ++chars;
  }
  return text.size();
}

int ByteToCursor(const std::string& text, const size_t byte_offset) {
  int chars = 0;
  const size_t limit = std::min(byte_offset, text.size());
  for (size_t i = 0; i < limit; ++i) {
    if (!IsContinuation(static_cast<unsigned char>(text[i]))) {
      ++chars;
    }
  }
  return chars;
}

void MentionDraft::Enable(Loader loader, const bool owner, std::string everyone_name) {
  enabled_ = true;
  loader_ = std::move(loader);
  owner_ = owner;
  everyone_name_ = std::move(everyone_name);
  ClosePopup();
}

void MentionDraft::Disable() {
  enabled_ = false;
  loader_ = nullptr;
  owner_ = false;
  Reset();
}

void MentionDraft::Reset() {
  mentions_.clear();
  last_text_.clear();
  ClosePopup();
}

void MentionDraft::ClosePopup() {
  open_ = false;
  rows_.clear();
  row_member_.clear();
  members_.clear();
}

bool MentionDraft::InsideMention(const size_t at_byte) const {
  return std::any_of(mentions_.begin(), mentions_.end(),
                     [&](const DraftMention& m) { return at_byte >= m.start && at_byte < m.end; });
}

std::optional<DraftEdit> MentionDraft::OnDraftChanged(const std::string& text, const int cursor_chars,
                                                      const bool committed_input, const bool composing) {
  if (!enabled_ || composing) {
    return std::nullopt;
  }
  const size_t cursor = CursorToByte(text, cursor_chars);

  std::optional<DraftEdit> rewrite;
  std::string current = text;
  size_t current_cursor = cursor;

  if (text != last_text_) {
    // The edit is [p, old_end) of the old text replaced by new_len bytes.
    size_t p = 0;
    const size_t max_p = std::min(last_text_.size(), text.size());
    while (p < max_p && last_text_[p] == text[p]) {
      ++p;
    }
    p = BackToBoundary(last_text_, BackToBoundary(text, p));
    size_t suffix = 0;
    while (suffix < last_text_.size() - p && suffix < text.size() - p &&
           last_text_[last_text_.size() - 1 - suffix] == text[text.size() - 1 - suffix]) {
      ++suffix;
    }
    // Keep the suffix on a character boundary of both texts.
    while (suffix > 0 && (IsContinuation(static_cast<unsigned char>(last_text_[last_text_.size() - suffix])) ||
                          IsContinuation(static_cast<unsigned char>(text[text.size() - suffix])))) {
      --suffix;
    }
    const size_t old_end = last_text_.size() - suffix;
    const size_t new_len = text.size() - suffix - p;
    const long delta = static_cast<long>(new_len) - static_cast<long>(old_end - p);

    std::vector<DraftMention> kept;
    size_t whole_delete_start = std::string::npos; // a mention deleted as a whole: the part before p still to remove
    size_t whole_delete_end = 0;
    for (DraftMention m : mentions_) {
      if (old_end <= m.start) { // before the mention (typing right before it too): shift
        m.start = static_cast<size_t>(static_cast<long>(m.start) + delta);
        m.end = static_cast<size_t>(static_cast<long>(m.end) + delta);
        kept.push_back(std::move(m));
      } else if (p >= m.end) { // after it, or typing right at its end
        kept.push_back(std::move(m));
      } else if (p <= m.start && old_end >= m.end) {
        // the text of the mention is gone with the edit
      } else if (new_len == 0 && old_end == m.end && p > m.start) {
        // Backspace at its end: the whole mention goes.
        whole_delete_start = m.start;
        whole_delete_end = p;
      }
      // else: typed or pasted inside it, or an edit that cuts into it: the record is void, the text stays.
    }
    if (whole_delete_start != std::string::npos) {
      const size_t removed = whole_delete_end - whole_delete_start;
      for (DraftMention& m : kept) {
        if (m.start >= whole_delete_end) {
          m.start -= removed;
          m.end -= removed;
        }
      }
      current = text.substr(0, whole_delete_start) + text.substr(whole_delete_end);
      current_cursor = whole_delete_start;
      rewrite = DraftEdit{current, ByteToCursor(current, current_cursor)};
    }
    mentions_ = std::move(kept);
    last_text_ = current;
  } else if (cursor == cursor_byte_ && !committed_input) {
    return std::nullopt;
  }

  // The query at the cursor: "@" at the start or after whitespace, then non-space characters up to the cursor.
  size_t at = std::string::npos;
  for (size_t i = current_cursor; i > 0; --i) {
    const char c = current[i - 1];
    if (IsSpace(c)) {
      break;
    }
    if (c == '@') {
      if (i - 1 == 0 || IsSpace(current[i - 2])) {
        at = i - 1;
      }
      break;
    }
  }
  const bool fresh_char = committed_input || rewrite.has_value();
  if (at == std::string::npos || InsideMention(at)) {
    ClosePopup();
    return rewrite;
  }
  if (open_ && at == at_byte_) {
    cursor_byte_ = current_cursor;
    Refresh(current.substr(at + 1, current_cursor - at - 1));
    return rewrite;
  }
  if (!fresh_char) {
    // A change nobody confirmed with a textinput (or a cursor jump into some "@x"): never opens.
    ClosePopup();
    return rewrite;
  }
  OpenFor(current, at, current_cursor);
  return rewrite;
}

void MentionDraft::OpenFor(const std::string& text, const size_t at_byte, const size_t cursor_byte) {
  members_ = loader_ ? loader_() : std::vector<MentionCandidate>{};
  open_ = true;
  at_byte_ = at_byte;
  cursor_byte_ = cursor_byte;
  Refresh(text.substr(at_byte + 1, cursor_byte - at_byte - 1));
}

void MentionDraft::Refresh(const std::string& query) {
  rows_.clear();
  row_member_.clear();
  const std::string needle = Fold(query);

  std::map<std::string, int> name_counts;
  for (const MentionCandidate& m : members_) {
    ++name_counts[m.name];
  }

  if (owner_ && !everyone_name_.empty() && Fold(everyone_name_).find(needle) != std::string::npos) {
    rows_.push_back({static_cast<int>(rows_.size()), everyone_name_, true});
    row_member_.push_back(-1);
  }

  std::vector<int> order;
  for (size_t i = 0; i < members_.size(); ++i) {
    if (Fold(members_[i].name).find(needle) != std::string::npos) {
      order.push_back(static_cast<int>(i));
    }
  }
  std::stable_sort(order.begin(), order.end(), [&](const int a, const int b) {
    const std::string fa = Fold(members_[a].name);
    const std::string fb = Fold(members_[b].name);
    return fa != fb ? fa < fb : members_[a].account_id < members_[b].account_id;
  });
  for (const int i : order) {
    std::string label = members_[i].name;
    if (name_counts[members_[i].name] > 1) {
      label += " · " + ShortRelayId(members_[i].account_id);
    }
    rows_.push_back({static_cast<int>(rows_.size()), std::move(label), false});
    row_member_.push_back(i);
  }
}

std::optional<DraftEdit> MentionDraft::Pick(const int row_index, const std::optional<std::string>& current_text) {
  if (!open_ || row_index < 0 || row_index >= static_cast<int>(rows_.size())) {
    return std::nullopt;
  }
  const int member = row_member_[row_index];
  const bool everyone = member < 0;
  const std::string name = everyone ? everyone_name_ : members_[member].name;
  const std::string inserted = "@" + name;
  const std::string replacement = inserted + " ";

  const size_t at = at_byte_;
  // Base: the text the records refer to. Characters typed at the cursor since then extend the query.
  const std::string& old_text = last_text_;
  const size_t old_query_end = cursor_byte_;
  std::string base = old_text;
  size_t query_end = old_query_end;
  if (current_text && current_text->size() > old_text.size()) {
    const std::string& now = *current_text;
    const size_t extra = now.size() - old_text.size();
    if (now.compare(0, old_query_end, old_text, 0, old_query_end) == 0 &&
        now.compare(old_query_end + extra, std::string::npos, old_text, old_query_end, std::string::npos) == 0) {
      base = now;
      query_end = old_query_end + extra;
    }
  }

  std::string text = base.substr(0, at) + replacement + base.substr(query_end);
  const long shift = static_cast<long>(text.size()) - static_cast<long>(old_text.size());
  for (DraftMention& m : mentions_) {
    if (m.start >= old_query_end) {
      m.start = static_cast<size_t>(static_cast<long>(m.start) + shift);
      m.end = static_cast<size_t>(static_cast<long>(m.end) + shift);
    }
  }
  const bool have_all = std::any_of(mentions_.begin(), mentions_.end(), [](const DraftMention& m) { return m.all; });
  if (mentions_.size() < kMaxDraftMentions && !(everyone && have_all)) {
    DraftMention record;
    record.account_id = everyone ? std::string() : members_[member].account_id;
    record.all = everyone;
    record.start = at;
    record.end = at + inserted.size();
    record.text = inserted;
    mentions_.push_back(std::move(record));
    std::sort(mentions_.begin(), mentions_.end(),
              [](const DraftMention& a, const DraftMention& b) { return a.start < b.start; });
  }
  last_text_ = text;
  ClosePopup();
  return DraftEdit{text, ByteToCursor(text, at + replacement.size())};
}

std::vector<DraftMention> MentionDraft::FinalizeForSend(const std::string& draft_text,
                                                        const std::string& sent_text) const {
  std::vector<DraftMention> out;
  if (!enabled_ || draft_text != last_text_) {
    return out;
  }
  size_t lead = 0;
  while (lead < draft_text.size() && IsSpace(draft_text[lead])) {
    ++lead;
  }
  bool have_all = false;
  for (const DraftMention& m : mentions_) {
    if (out.size() >= kMaxDraftMentions || m.start < lead || m.end <= m.start) {
      continue;
    }
    const size_t start = m.start - lead;
    const size_t end = m.end - lead;
    if (end > sent_text.size() || sent_text.compare(start, end - start, m.text) != 0 || m.text.empty() ||
        m.text[0] != '@') {
      continue;
    }
    if (m.all) {
      if (have_all) {
        continue;
      }
      have_all = true;
    }
    DraftMention fixed = m;
    fixed.start = start;
    fixed.end = end;
    out.push_back(std::move(fixed));
  }
  return out;
}

} // namespace pbr
