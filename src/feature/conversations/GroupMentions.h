#pragma once

#include "common/Lifetime.h"
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

/** The English everyone word: the everyone line always answers to it, whatever language it inserts. */
inline constexpr const char* kEveryoneMentionWord = "everyone";

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
  /** What the line shows; the inserted token is `@` + this. Everyone: the word of the user's language. */
  std::string name;
  /** Set only when another member in the group has the same name: the line shows it after the name. */
  std::string short_id;

  std::string Token() const { return "@" + name; }
  /** What the picker shows: the name, then ` · <short id>` when names collide. */
  std::string Label() const { return short_id.empty() ? name : name + " · " + short_id; }
  bool operator==(const MentionCandidate&) const = default;
};

/**
 * Every line the picker can show for a group, unfiltered: the everyone line first when this device is the group's
 * owner, then the other members (never ourselves) by name. Reading it costs the roster, the cards and the
 * contacts, so the picker reads it once when it opens and filters it with FilterMentionCandidates. Members that
 * share a name (ASCII case ignored) carry a `short_id`.
 */
std::vector<MentionCandidate> AllMentionCandidates(const std::vector<GroupRosterMember>& roster,
                                                   const std::string& local_account, bool local_is_owner,
                                                   const std::function<std::string(const std::string& account_id)>& public_name,
                                                   const std::string& everyone_word);

/** The lines of `all` whose name contains `query` (ASCII case ignored); the everyone line also answers to "everyone". */
std::vector<MentionCandidate> FilterMentionCandidates(const std::vector<MentionCandidate>& all,
                                                      const std::string& query);

/**
 * The picker's lines for the open group chat, built ahead of time so typing `@` and each keystroke after it only
 * filter in memory (no roster, card or contact read on the UI thread). The UI thread owns it.
 *
 * - `Open` starts a load when a group chat opens; `Drop` forgets the group (the chat changed); a load that finishes
 *   after a `Drop` is ignored (`pp::Lifetime`).
 * - `Changed` is called when the chat refreshes: it reloads only if `version` (GroupRosterVersion: a membership
 *   event was applied here) is not the one the lines were read at.
 * - `Typed` (an `@` was typed): reloads in the background when the lines are older than kStaleMs — names arrive by
 *   paths with no signal — or a change was held back; the lines already held answer at once.
 * - `SetOnLoaded`: told (UI thread) whenever a load finishes, so an open picker or one waiting for its first lines
 *   refreshes, including after a drop and reopen.
 */
class MentionCandidatePreload {
public:
  using Lines = std::vector<MentionCandidate>;
  /** Runs `read` off the UI thread and calls `done` with its result on the UI thread. */
  using Runner = std::function<void(std::function<Lines()> read, std::function<void(Lines)> done)>;
  /** The unfiltered lines of a group chat (reads the roster, cards and contacts). Runs on a worker. */
  using Reader = std::function<Lines(const std::string& thread_id)>;
  /**
   * Called on the UI thread at each load to make the Reader that load runs: it must carry what the worker needs by
   * value (a facade pointer), never the picker or presenter, which the worker may outlive.
   */
  using ReaderFactory = std::function<Reader()>;
  static constexpr int64_t kStaleMs = 30000;

  MentionCandidatePreload(Runner run, ReaderFactory make_reader, std::function<uint64_t()> version,
                          std::function<int64_t()> now_ms)
      : run_(std::move(run)),
        make_reader_(std::move(make_reader)),
        version_(std::move(version)),
        now_ms_(std::move(now_ms)) {}

  void Open(const std::string& thread_id);
  void Changed();
  void Typed();
  void Drop();

  const std::string& ThreadId() const { return thread_id_; }
  bool Ready() const { return ready_; }
  Lines Filter(const std::string& query) const { return FilterMentionCandidates(all_, query); }
  void SetOnLoaded(std::function<void()> callback) { on_loaded_ = std::move(callback); }

private:
  void Start();
  void Loaded(const std::string& thread_id, Lines lines);

  Runner run_;
  ReaderFactory make_reader_;
  std::function<uint64_t()> version_;
  std::function<int64_t()> now_ms_;
  pp::Lifetime lifetime_;
  std::string thread_id_;
  Lines all_;
  bool ready_ = false;
  bool loading_ = false;
  /** The membership changed while a load ran: read again when it is done. */
  bool pending_ = false;
  uint64_t loaded_version_ = 0;
  uint64_t loading_version_ = 0;
  int64_t last_started_ms_ = 0;
  std::function<void()> on_loaded_;
};

/**
 * The picker's lines: the everyone line first when this device is the group's owner (`local_is_owner`: the owner
 * the roster names), then the other members (never ourselves) by name. `everyone_word` is what that line
 * inserts after the `@` (the user's language: `@所有人`, `@everyone`). `query` (the text typed after the `@`)
 * keeps the lines whose name contains it, ignoring ASCII case; the everyone line also answers to "everyone"
 * whatever its word. Members that share a name carry a `short_id` (decided over the whole group, not the query).
 */
std::vector<MentionCandidate> MentionCandidates(const std::vector<GroupRosterMember>& roster,
                                                const std::string& local_account, bool local_is_owner,
                                                const std::function<std::string(const std::string& account_id)>& public_name,
                                                const std::string& query, const std::string& everyone_word);

/**
 * The `@` the user just typed that opens the picker: `after` is `before` with one `@` added, at the start of the
 * text or after whitespace. Its byte offset; nullopt for any other edit.
 */
std::optional<size_t> TypedMentionTrigger(const std::string& before, const std::string& after);

/**
 * While the picker is open on the `@` at byte `at`: what was typed after it, up to the caret (a byte offset in
 * `text`; the text after the caret is not part of the query, so an `@` typed in front of a word leaves the word
 * alone). nullopt once that `@` is gone, the caret is not after it, or whitespace came in between (the user wrote
 * something else), so the picker closes.
 */
std::optional<std::string> MentionQueryAt(const std::string& text, size_t at, size_t caret);

/**
 * Choosing a line: `@` + query (from `at` to `caret`) replaced by `token` and a space; `text` after the caret is
 * kept. The result's `caret` is the byte after the token and its space.
 */
struct MentionInsert {
  std::string text;
  size_t caret = 0;
};
MentionInsert InsertMention(const std::string& text, size_t at, size_t caret, const std::string& token);

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
 * Forget the picks whose token the draft no longer holds as often as it was picked (deleted, or edited): per
 * token, when there are more picks than occurrences the oldest picks go. Call it on a draft change, so a deleted
 * pick cannot take the place of a later one with the same name.
 */
void PruneRecordedMentions(const std::string& text, std::vector<ComposerMention>& recorded);

/**
 * The spans to send `text` with: each recorded mention takes the first occurrence of its token that no other
 * has claimed, whatever order the picks were made in (picks of the same token, so same-name members, go in pick
 * order); the span covers exactly the token. A token edited away (not found, or run into a longer word) is
 * dropped, and so is a second mention of the same account or a second `@everyone` (the receiver would reject
 * the whole message, G015). In byte order; ranges are UTF-8 bytes. Two same-name members can still swap when
 * the user moves one of their tokens past the other, or types one of the names by hand before a picked one.
 */
std::vector<TextSpan> MentionSpansForDraft(const std::string& text, const std::vector<ComposerMention>& recorded);

/** What deciding a highlight needs: the group's current roster, its owner, and who we are. */
struct MentionRoster {
  std::set<std::string> members;
  /** The owner the roster names (G012). */
  std::string owner;
  std::string local_account;
};

/**
 * `spans` without the mentions of anyone not on `roster`, and without an everyone span unless this account is the
 * group's owner: a pick made while a member was there can be stale at send (they left, were removed).
 */
std::vector<TextSpan> KeepMentionsOnRoster(std::vector<TextSpan> spans, const MentionRoster& roster);

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
