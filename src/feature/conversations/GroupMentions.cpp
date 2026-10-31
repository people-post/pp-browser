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

std::vector<MentionCandidate> MentionCandidates(const std::vector<GroupRosterMember>& roster,
                                                const std::string& local_account, const bool local_is_owner,
                                                const std::function<std::string(const std::string& account_id)>& public_name,
                                                const std::string& query, const std::string& everyone_word) {
  const std::string lowered = AsciiLower(query);
  std::vector<MentionCandidate> all;
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
    ++name_counts[candidate.name];
    all.push_back(std::move(candidate));
  }
  std::vector<MentionCandidate> members;
  for (MentionCandidate& candidate : all) {
    if (!NameMatches(candidate.name, lowered)) {
      continue;
    }
    if (name_counts[candidate.name] > 1) {
      candidate.short_id = ShortRelayId(candidate.account_id);
    }
    members.push_back(std::move(candidate));
  }
  std::stable_sort(members.begin(), members.end(), [](const MentionCandidate& a, const MentionCandidate& b) {
    const std::string la = AsciiLower(a.name);
    const std::string lb = AsciiLower(b.name);
    return la != lb ? la < lb : a.account_id < b.account_id;
  });
  std::vector<MentionCandidate> lines;
  if (local_is_owner && !everyone_word.empty() &&
      (NameMatches(everyone_word, lowered) || NameMatches(kEveryoneMentionWord, lowered))) {
    lines.push_back({TextSpanKind::Everyone, {}, everyone_word, {}});
  }
  lines.insert(lines.end(), members.begin(), members.end());
  return lines;
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

std::vector<TextSpan> MentionSpansForDraft(const std::string& text, const std::vector<ComposerMention>& recorded) {
  std::vector<TextSpan> spans;
  std::set<std::string> accounts;
  bool everyone = false;
  size_t from = 0;
  for (const ComposerMention& mention : recorded) {
    if (mention.token.size() < 2 || mention.token.front() != '@') {
      continue;
    }
    const bool is_everyone = mention.kind == TextSpanKind::Everyone;
    if (!is_everyone && (mention.kind != TextSpanKind::Mention || mention.account_id.empty())) {
      continue;
    }
    const size_t pos = FindToken(text, mention.token, from);
    if (pos == std::string::npos) {
      continue;  // edited away: the text stays as typed
    }
    from = pos + mention.token.size();
    if (is_everyone ? std::exchange(everyone, true) : !accounts.insert(mention.account_id).second) {
      continue;  // a second mention of the same: plain text (the receiver would reject the message)
    }
    if (!is_everyone && accounts.size() > text_spans::kMaxMentions) {
      continue;
    }
    TextSpan span;
    span.kind = static_cast<uint8_t>(mention.kind);
    span.start = static_cast<uint32_t>(pos);
    span.end = static_cast<uint32_t>(from);
    span.account_id = is_everyone ? std::string() : mention.account_id;
    spans.push_back(std::move(span));
    if (spans.size() == text_spans::kMaxSpans) {
      break;
    }
  }
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
