#pragma once

#include "lib/messaging/GroupTypes.h"
#include "lib/messaging/TextSpans.h"
#include "lib/people/ContactTypes.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

// Mentions in a group chat (group-chat G015), the parts the GUI needs without a GUI: what the composer's
// `@` picker lists and inserts, the spans a draft is sent with, and which spans a transcript highlights.

/** The token the picker puts for `@everyone` (the owner's to use). */
inline constexpr const char* kEveryoneMentionToken = "@everyone";

/**
 * The name a member is mentioned by: their PUBLIC nickname — the one on the account card the roster carried,
 * else the one the saved contact holds from the directory — else the short account id. Never the user's
 * private remark for that contact (`contact.local.display_name`): every member would read it.
 */
std::string MemberPublicName(const std::string& account_id, const std::string& card_nickname,
                             const std::optional<Contact>& contact);

/** One line of the composer's `@` picker. */
struct MentionCandidate {
  TextSpanKind kind = TextSpanKind::Mention;
  /** Mention: the member's account id. */
  std::string account_id;
  /** What the line shows; the inserted token is `@` + this. */
  std::string name;

  std::string Token() const { return kind == TextSpanKind::Everyone ? kEveryoneMentionToken : "@" + name; }
  bool operator==(const MentionCandidate&) const = default;
};

/**
 * The picker's lines: `@everyone` first when this device is the group's owner (`local_is_owner`: the owner the
 * roster names), then the other members (never ourselves) by name. `query` (the text typed after the `@`)
 * keeps the lines whose name contains it, ignoring ASCII case.
 */
std::vector<MentionCandidate> MentionCandidates(const std::vector<GroupRosterMember>& roster,
                                                const std::string& local_account, bool local_is_owner,
                                                const std::function<std::string(const std::string& account_id)>& public_name,
                                                const std::string& query);

/**
 * The `@` the user just typed that opens the picker: `after` is `before` with one `@` added, at the start of the
 * text or after whitespace. Its byte offset; nullopt for any other edit.
 */
std::optional<size_t> TypedMentionTrigger(const std::string& before, const std::string& after);

/**
 * While the picker is open on the `@` at byte `at`: the word typed after it (up to the end of the text or the
 * next whitespace). nullopt once that `@` is gone, or the word is finished (whitespace typed after it): the
 * user wrote something else, so the picker closes.
 */
std::optional<std::string> MentionQueryAt(const std::string& text, size_t at);

/** Choosing a line: `@` + query at byte `at` replaced by `token` and a space; `caret` is the byte after them. */
struct MentionInsert {
  std::string text;
  size_t caret = 0;
};
MentionInsert InsertMention(const std::string& text, size_t at, const std::string& token);

/** A mention the composer inserted: what to mark when the draft is sent. */
struct ComposerMention {
  TextSpanKind kind = TextSpanKind::Mention;
  /** Mention: the member's account id. */
  std::string account_id;
  /** As inserted: `@` + the public nickname, or `@everyone`. */
  std::string token;

  bool operator==(const ComposerMention&) const = default;
};

/**
 * The spans to send `text` with: for each recorded mention, in the order it was inserted, its token searched
 * after the previous one's; the span covers exactly the token. A token edited away (not found, or run into
 * a longer word) is dropped, and so is a second mention of the same account or a second `@everyone` (the
 * receiver would reject the whole message, G015). Ranges are UTF-8 bytes.
 */
std::vector<TextSpan> MentionSpansForDraft(const std::string& text, const std::vector<ComposerMention>& recorded);

/**
 * Whether sending `text` with these spans asks the AI. A text that starts `@ai` does, unless the composer
 * recorded a member mention at its first character: that `@ai` is a person who picked this name (G015). A typed
 * `@ai` with no such span (and `@ai+`, `@ai share` ...) still asks the AI.
 */
bool AsksTheAi(const std::string& text, const std::vector<TextSpan>& spans);

/** What deciding a highlight needs: the group's current roster, its owner, and who we are. */
struct MentionRoster {
  std::set<std::string> members;
  /** The owner the roster names (G012). */
  std::string owner;
  std::string local_account;
};

/** A highlighted range of a message's text. */
struct MentionRun {
  size_t begin = 0;
  size_t end = 0;
  TextSpanKind kind = TextSpanKind::Mention;
  /** Mention: the member's account id. */
  std::string account_id;

  bool operator==(const MentionRun&) const = default;
};

/**
 * How much of a message's text its mentions may mark: a reply that quotes another message (QuoteReply.h)
 * marks only its own words — the quote under it is a copy, and a copy drops mentions (G015).
 */
size_t MentionTextLimit(const std::string& text);

/**
 * The spans of a message to highlight now (G015: eligibility is checked at display, so a late roster restores
 * them): a mention of a current member, `@everyone` only from the owner. `sender_account` is the message's
 * sender (ours: `roster.local_account`). Only spans inside the first `text_limit` bytes count (a reply's own
 * words, not the quote under it); a range that is not valid in `text` is not highlighted. In byte order.
 */
std::vector<MentionRun> EligibleMentionRuns(const std::string& text, size_t text_limit,
                                            const std::vector<TextSpan>& spans, const MentionRoster& roster,
                                            const std::string& sender_account);

} // namespace pbr
