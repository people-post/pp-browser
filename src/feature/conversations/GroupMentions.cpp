#include "feature/conversations/GroupMentions.h"

#include "common/Utilities.h"
#include "common/directory/ShortRelayId.h"
#include "lib/messaging/QuoteReply.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <utility>

namespace pbr {
namespace {

bool IsAsciiSpace(const char ch) {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

/** An ASCII letter, digit or `_`: a name or an e-mail address goes on through it. */
bool IsAsciiWordChar(const char ch) {
  const auto byte = static_cast<unsigned char>(ch);
  return byte < 0x80 && (std::isalnum(byte) || ch == '_');
}

std::string AsciiLower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](const unsigned char ch) {
    return ch < 0x80 ? static_cast<char>(std::tolower(ch)) : static_cast<char>(ch);
  });
  return text;
}

bool NameMatches(const std::string& name, const std::string& lowered_query) {
  return lowered_query.empty() || AsciiLower(name).find(lowered_query) != std::string::npos;
}

/**
 * `token` in `text` from byte `from`, as a word of its own: not glued to a word before it (`a@bob` is an
 * address), nor run on into a longer ASCII name after it (`@Bobby` is not `@Bob`). A name in another script
 * has no word boundary to check after it.
 */
size_t FindToken(const std::string& text, const std::string& token, size_t from) {
  while (from <= text.size()) {
    const size_t pos = text.find(token, from);
    if (pos == std::string::npos) {
      return std::string::npos;
    }
    const size_t end = pos + token.size();
    const bool clear_before = pos == 0 || !IsAsciiWordChar(text[pos - 1]);
    const bool clear_after =
        end == text.size() || !IsAsciiWordChar(text[end]) || !IsAsciiWordChar(token.back());
    if (clear_before && clear_after) {
      return pos;
    }
    from = pos + 1;
  }
  return std::string::npos;
}

} // namespace

std::string MemberPublicName(const std::string& account_id, const std::string& card_nickname,
                             const std::optional<Contact>& contact) {
  // The private remark (contact->local.display_name) is deliberately not read here.
  if (std::string card = util::Trim(card_nickname); !card.empty()) {
    return card;
  }
  if (contact) {
    if (std::string directory = util::Trim(contact->remote.nickname); !directory.empty()) {
      return directory;
    }
  }
  return ShortRelayId(account_id);
}

std::vector<MentionCandidate> AllMentionCandidates(const std::vector<GroupRosterMember>& roster,
                                                   const std::string& local_account, const bool local_is_owner,
                                                   const std::function<std::string(const std::string& account_id)>& public_name,
                                                   const std::string& everyone_word) {
  std::vector<MentionCandidate> members;
  std::map<std::string, int> name_counts;
  for (const GroupRosterMember& member : roster) {
    if (member.member_identity.empty() || member.member_identity == local_account) {
      continue;
    }
    MentionCandidate candidate;
    candidate.account_id = member.member_identity;
    candidate.name = public_name ? public_name(member.member_identity) : ShortRelayId(member.member_identity);
    if (candidate.name.empty()) {
      continue;
    }
    ++name_counts[AsciiLower(candidate.name)];
    members.push_back(std::move(candidate));
  }
  for (MentionCandidate& candidate : members) {
    if (name_counts[AsciiLower(candidate.name)] > 1) {
      candidate.short_id = ShortRelayId(candidate.account_id);
    }
  }
  std::stable_sort(members.begin(), members.end(), [](const MentionCandidate& a, const MentionCandidate& b) {
    const std::string la = AsciiLower(a.name);
    const std::string lb = AsciiLower(b.name);
    return la != lb ? la < lb : a.account_id < b.account_id;
  });
  std::vector<MentionCandidate> lines;
  if (local_is_owner) {
    lines.push_back({TextSpanKind::Everyone, {}, everyone_word, {}});
  }
  lines.insert(lines.end(), members.begin(), members.end());
  return lines;
}

std::vector<MentionCandidate> FilterMentionCandidates(const std::vector<MentionCandidate>& all,
                                                      const std::string& query) {
  const std::string lowered = AsciiLower(query);
  std::vector<MentionCandidate> lines;
  for (const MentionCandidate& candidate : all) {
    const bool everyone = candidate.kind == TextSpanKind::Everyone;
    if (NameMatches(candidate.name, lowered) || (everyone && NameMatches(kEveryoneMentionWord, lowered))) {
      lines.push_back(candidate);
    }
  }
  return lines;
}

std::vector<MentionCandidate> MentionCandidates(const std::vector<GroupRosterMember>& roster,
                                                const std::string& local_account, const bool local_is_owner,
                                                const std::function<std::string(const std::string& account_id)>& public_name,
                                                const std::string& query, const std::string& everyone_word) {
  return FilterMentionCandidates(
      AllMentionCandidates(roster, local_account, local_is_owner, public_name, everyone_word), query);
}

void MentionCandidatePreload::Open(const std::string& thread_id) {
  if (thread_id.empty()) {
    return;
  }
  if (thread_id != thread_id_) {
    Drop();
    thread_id_ = thread_id;
  }
  if (!ready_ && !loading_) {
    Start();
  }
}

void MentionCandidatePreload::Changed() {
  if (thread_id_.empty() || (!ready_ && !loading_)) {
    return;  // nothing loaded for this chat yet: Open / Typed start it
  }
  const uint64_t version = version_();
  if (loading_) {
    pending_ = pending_ || version != loading_version_;
  } else if (version != loaded_version_) {
    Start();
  }
}

void MentionCandidatePreload::Typed() {
  if (thread_id_.empty() || loading_) {
    return;
  }
  if (!ready_ || pending_ || version_() != loaded_version_ || now_ms_() - last_started_ms_ >= kStaleMs) {
    Start();  // answers from what is held meanwhile; the open picker refreshes when this lands
  }
}

void MentionCandidatePreload::Drop() {
  lifetime_.Invalidate();
  thread_id_.clear();
  all_.clear();
  ready_ = false;
  loading_ = false;
  pending_ = false;
}

void MentionCandidatePreload::Start() {
  loading_ = true;
  pending_ = false;
  loading_version_ = version_();
  last_started_ms_ = now_ms_();
  const std::string thread_id = thread_id_;
  run_([read = make_reader_(), thread_id]() { return read ? read(thread_id) : Lines{}; },
       lifetime_.Bind([this, thread_id](Lines lines) { Loaded(thread_id, std::move(lines)); }));
}

void MentionCandidatePreload::Loaded(const std::string& thread_id, Lines lines) {
  if (thread_id != thread_id_) {
    return;
  }
  all_ = std::move(lines);
  ready_ = true;
  loading_ = false;
  loaded_version_ = loading_version_;
  if (pending_) {
    Start();  // the membership changed while this load ran
  }
  if (on_loaded_) {
    on_loaded_();
  }
}

std::optional<size_t> TypedMentionTrigger(const std::string& before, const std::string& after) {
  if (after.size() != before.size() + 1) {
    return std::nullopt;
  }
  size_t at = 0;
  while (at < before.size() && before[at] == after[at]) {
    ++at;
  }
  if (after[at] != '@' || after.compare(at + 1, std::string::npos, before, at, std::string::npos) != 0) {
    return std::nullopt;
  }
  // At the start, after whitespace, or after text in another script (Chinese has no spaces between words);
  // not inside an ASCII word, so an e-mail address does not open the picker.
  if (at == 0 || IsAsciiSpace(after[at - 1]) || static_cast<unsigned char>(after[at - 1]) >= 0x80) {
    return at;
  }
  return std::nullopt;
}

std::optional<std::string> MentionQueryAt(const std::string& text, const size_t at, const size_t caret) {
  if (at >= text.size() || text[at] != '@' || caret <= at) {
    return std::nullopt;
  }
  std::string query = text.substr(at + 1, std::min(caret, text.size()) - at - 1);
  if (std::any_of(query.begin(), query.end(), IsAsciiSpace)) {
    return std::nullopt;  // whitespace after the @: the user typed on
  }
  return query;
}

MentionInsert InsertMention(const std::string& text, const size_t at, const size_t caret, const std::string& token) {
  if (at >= text.size() || text[at] != '@') {
    return {text, text.size()};
  }
  const size_t end = std::clamp(caret, at + 1, text.size());
  MentionInsert out;
  out.text = text.substr(0, at) + token;
  if (end < text.size() && text[end] == ' ') {
    out.caret = out.text.size() + 1;  // the space already there separates it
  } else {
    out.text += ' ';
    out.caret = out.text.size();
  }
  out.text += text.substr(end);
  return out;
}

namespace {

bool Overlaps(const std::vector<std::pair<size_t, size_t>>& claimed, const size_t begin, const size_t end) {
  return std::any_of(claimed.begin(), claimed.end(),
                     [begin, end](const auto& range) { return begin < range.second && range.first < end; });
}

}  // namespace

void PruneRecordedMentions(const std::string& text, std::vector<ComposerMention>& recorded) {
  std::map<std::pair<TextSpanKind, std::string>, std::vector<size_t>> by_token;
  for (size_t i = 0; i < recorded.size(); ++i) {
    by_token[{recorded[i].kind, recorded[i].token}].push_back(i);
  }
  std::vector<bool> drop(recorded.size(), false);
  for (const auto& [key, indexes] : by_token) {
    size_t occurrences = 0;
    for (size_t from = 0; !key.second.empty();) {
      const size_t pos = FindToken(text, key.second, from);
      if (pos == std::string::npos) {
        break;
      }
      ++occurrences;
      from = pos + key.second.size();
    }
    for (size_t i = 0; i + occurrences < indexes.size(); ++i) {
      drop[indexes[i]] = true;  // more picks than tokens left: the oldest ones are the ones deleted
    }
  }
  size_t kept = 0;
  for (size_t i = 0; i < recorded.size(); ++i) {
    if (!drop[i]) {
      if (kept != i) {
        recorded[kept] = std::move(recorded[i]);
      }
      ++kept;
    }
  }
  recorded.resize(kept);
}

std::vector<TextSpan> MentionSpansForDraft(const std::string& text, const std::vector<ComposerMention>& recorded) {
  // Longest token first (a pick of `@张` must not take the front of `@张三`), picks of equal tokens in pick order:
  // each takes the first occurrence nobody has claimed, wherever the other picks sit in the text.
  std::vector<const ComposerMention*> order;
  for (const ComposerMention& mention : recorded) {
    order.push_back(&mention);
  }
  std::stable_sort(order.begin(), order.end(), [](const ComposerMention* a, const ComposerMention* b) {
    return a->token.size() > b->token.size();
  });
  std::vector<TextSpan> spans;
  std::vector<std::pair<size_t, size_t>> claimed;
  std::set<std::string> accounts;
  bool everyone = false;
  for (const ComposerMention* mention : order) {
    if (mention->token.size() < 2 || mention->token.front() != '@') {
      continue;
    }
    const bool is_everyone = mention->kind == TextSpanKind::Everyone;
    if (!is_everyone && (mention->kind != TextSpanKind::Mention || mention->account_id.empty())) {
      continue;
    }
    size_t pos = FindToken(text, mention->token, 0);
    while (pos != std::string::npos && Overlaps(claimed, pos, pos + mention->token.size())) {
      pos = FindToken(text, mention->token, pos + 1);
    }
    if (pos == std::string::npos) {
      continue;  // edited away: the text stays as typed
    }
    const size_t end = pos + mention->token.size();
    if (is_everyone ? std::exchange(everyone, true) : !accounts.insert(mention->account_id).second) {
      continue;  // a second mention of the same: plain text (the receiver would reject the message)
    }
    if (!is_everyone && accounts.size() > text_spans::kMaxMentions) {
      continue;
    }
    claimed.emplace_back(pos, end);
    TextSpan span;
    span.kind = static_cast<uint8_t>(mention->kind);
    span.start = static_cast<uint32_t>(pos);
    span.end = static_cast<uint32_t>(end);
    span.account_id = is_everyone ? std::string() : mention->account_id;
    spans.push_back(std::move(span));
    if (spans.size() == text_spans::kMaxSpans) {
      break;
    }
  }
  std::sort(spans.begin(), spans.end(), [](const TextSpan& a, const TextSpan& b) { return a.start < b.start; });
  return spans;
}

std::vector<TextSpan> KeepMentionsOnRoster(std::vector<TextSpan> spans, const MentionRoster& roster) {
  spans.erase(std::remove_if(spans.begin(), spans.end(),
                             [&roster](const TextSpan& span) {
                               if (span.Is(TextSpanKind::Mention)) {
                                 return roster.members.count(span.account_id) == 0;
                               }
                               if (span.Is(TextSpanKind::Everyone)) {
                                 return roster.owner.empty() || roster.owner != roster.local_account;
                               }
                               return false;
                             }),
              spans.end());
  return spans;
}

size_t MentionTextLimit(const std::string& text) {
  const std::optional<QuoteReplyParts> parts = SplitQuoteReply(text);
  return parts ? parts->reply.size() : text.size();
}

std::vector<MentionRun> EligibleMentionRuns(const std::string& text, const size_t text_limit,
                                            const std::vector<TextSpan>& spans, const MentionRoster& roster,
                                            const std::string& sender_account) {
  std::vector<MentionRun> runs;
  for (const TextSpan& span : text_spans::KeepValidRanges(text, spans)) {
    if (span.end > text_limit) {
      continue;
    }
    if (span.Is(TextSpanKind::Mention) && roster.members.count(span.account_id) > 0) {
      runs.push_back({span.start, span.end, TextSpanKind::Mention, span.account_id});
    } else if (span.Is(TextSpanKind::Everyone) && !roster.owner.empty() && sender_account == roster.owner) {
      runs.push_back({span.start, span.end, TextSpanKind::Everyone, {}});
    }
  }
  std::sort(runs.begin(), runs.end(), [](const MentionRun& a, const MentionRun& b) { return a.begin < b.begin; });
  return runs;
}

} // namespace pbr
