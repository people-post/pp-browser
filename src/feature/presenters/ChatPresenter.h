#pragma once

#include "feature/presenters/GroupLinkText.h"
#include "common/Error.h"
#include "common/Module.h"
#include "feature/conversations/GroupMentions.h"
#include "feature/presenters/SessionMenu.h"
#include "lib/messaging/GroupTypes.h"
#include "lib/messaging/IThreadLocalPrefsStore.h"
#include "lib/people/ContactTypes.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

class ConversationsFacade;
struct Thread;

/** One row of the chats list, as the user sees it (no RmlUi types). */
struct ChatSessionRow {
  std::string id;
  std::string title;
  /** The title is the stored default of a new AI chat: the renderer shows it in the UI language. */
  bool title_is_default_ai = false;
  std::string preview;
  /** The thread has no message text yet: `preview` holds the localised placeholder instead of an empty second line. */
  bool preview_empty = false;
  /** Visual chat type: ai | private | public | group. */
  std::string kind;
  int unread_count = 0;
  /** Marked unread by hand with no real unread count: a dot, not a number. */
  bool unread_dot = false;
  /** An unread message here mentions us (G015): the row says "You were mentioned". */
  bool mentioned = false;
  /** How many unread messages here mention us (Home's Mentions widget). */
  int mention_count = 0;
  bool pinned = false;
  bool muted = false;
  std::string date_label;
  /** Last activity, unix ms (Home's Recent chats and Favorites order by it, whatever is pinned). */
  int64_t updated_at = 0;
  bool active = false;
  /** A direct chat with someone who is not a saved contact (so not a blocked one either): a possible new friend. */
  bool from_stranger = false;
  /** A direct chat with a saved contact this device blocked: the row says so instead of the last message. */
  bool blocked = false;
  /** Letter and colour index (0..7) of the round avatar; the title decides both. */
  std::string avatar_letter;
  int avatar_tone = 0;
  /** Local file of the person's cached profile photo (direct chats with a saved contact); empty: show the letter. */
  std::string icon_path;
  /** The saved contact a direct chat is with; empty for anyone else, groups and the AI. */
  std::string contact_id;

  bool operator==(const ChatSessionRow&) const = default;
};

/** The chats list: pinned first, then by activity; archived chats are a separate list. */
struct ChatListScreen {
  /** Human chats (Direct/Group) — the 聊天 tab. AI threads are split out into `ai_rows`. */
  std::vector<ChatSessionRow> rows;
  /** AI conversations (ThreadKind::Ai, listed only) — the AI 助手 page. Same row shape as `rows`. */
  std::vector<ChatSessionRow> ai_rows;
  int archived_count = 0;
  /** The archived list is showing (back to the main list once nothing is archived). */
  bool showing_archived = false;
  /** Archived AI conversations (listed only): their own count and view, apart from the human chats' above. */
  int archived_count_ai = 0;
  bool showing_archived_ai = false;

  bool operator==(const ChatListScreen&) const = default;
};

/** Which list a thread belongs to for the current archive views: human chats and AI conversations never mix. */
enum class ChatListBucket { Human, Ai, Hidden };
ChatListBucket ClassifyForList(bool is_ai, bool listed, bool archived, bool showing_archived, bool showing_archived_ai);

/** One thread as the archive views see it. */
struct ArchiveItem {
  bool is_ai = false;
  bool listed = true;
  bool archived = false;
};
/** The two archive counts (human chats; listed AI conversations) and each view flag, reset once its own count is 0. */
struct ArchiveViews {
  int archived_count = 0;
  int archived_count_ai = 0;
  bool showing_archived = false;
  bool showing_archived_ai = false;
};
ArchiveViews ResolveArchiveViews(const std::vector<ArchiveItem>& items, bool showing_archived, bool showing_archived_ai);

/** What closing a chat does (the GUI asks before it does it). */
struct CloseChatPlan {
  enum class Kind {
    /** Delete the conversation (a person's; `DeleteAi`: the agent's). */
    Delete,
    DeleteAi,
    /** Leave the group (the owner stays). */
    Leave,
    /** Drop the group here only: a member's orphaned group — no owner on the roster, or nobody reachable. */
    Dismiss,
    /** The owner leaves: hand the group to one of `successors` (reachable members), or dissolve it. */
    LeaveAsOwner,
    /** The owner, with nobody to hand the group to (solo, or only unreachable members): dissolve it for everyone. */
    Dissolve,
  };
  Kind kind = Kind::Delete;
  std::string group_id;
  std::vector<std::string> successors;
};
const char* CloseChatPlanKindName(CloseChatPlan::Kind kind);

/** Someone the owner may hand the group to when leaving (reachable, on the roster, not this device). */
struct OwnerLeaveCandidate {
  std::string identity;
  std::string name;
  /** Seed for the letter-avatar colour (the person's account id when they are a contact). */
  std::string stable_id;

  bool operator==(const OwnerLeaveCandidate&) const = default;
};

/**
 * The phone's "choose a new owner" sheet (DEV/PLAN-ui-batch8-group-pages.md 3). Empty `thread_id`: the owner is not
 * leaving this chat (not a group, not the owner, dissolved). No candidates: nobody can take over now, so the sheet
 * offers only Dissolve.
 */
struct OwnerLeaveSheet {
  std::string thread_id;
  std::string group_id;
  std::vector<OwnerLeaveCandidate> candidates;

  bool operator==(const OwnerLeaveSheet&) const = default;
};
/** From the close plan (LeaveAsOwner or Dissolve), each successor named by `name_of`, sorted by name. */
OwnerLeaveSheet BuildOwnerLeaveSheet(const std::string& thread_id, const CloseChatPlan& plan,
                                     const std::function<std::string(const std::string& identity)>& name_of);
/** The confirm, asked once after a pick: "Make {name} the owner and exit the group?". */
std::string HandOverConfirmText(const std::string& name);
/** The one picked left or went out of reach meanwhile: "{name} is no longer in the group. Choose someone else." */
std::string HandOverGoneText(const std::string& name);
/** `successor` can still take the group over, by a close plan read now. */
bool HandOverStillPossible(const CloseChatPlan& plan, const std::string& successor);

/** A group's side of the open chat's peer sheet. */
struct ChatGroupSheet {
  /** This device owns the group. */
  bool is_owner = false;
  /** G017: the preset lets us rename the group for everyone (Open: any member; Managed: admins and the owner). */
  bool may_rename_for_everyone = false;
  /** The group's preset; the owner switches it ("Who can add people and rename"). */
  GroupPreset preset = GroupPreset::Open;
  bool owner_unreachable = false;
  /** Members the owner may remove for being unreachable (never this device): identity, then the name to show. */
  std::vector<std::pair<std::string, std::string>> unreachable_members;
};

/** One person on the group's member list, before any text is chosen for the badges. */
struct GroupMemberRow {
  std::string identity;
  /** Contact name, else the server nickname, else the short account id. */
  std::string title;
  /** Seed for the letter-avatar colour: the contact's account id, else the member identity. */
  std::string stable_id;
  /** The saved contact's id; empty for someone who is not a contact yet. */
  std::string contact_id;
  bool is_owner = false;
  /** G017: an admin the owner appointed (from the owner-signed roster). */
  bool is_admin = false;
  bool unreachable = false;
  /** Invited by this device, not on the roster yet (not a member; not counted in "N members"). */
  bool invited = false;
  /** Invited more than an hour ago and still not joined. */
  bool invited_stale = false;
  /** G019 5: who brought them in (an account), and that person's name as shown ("added by {name}"). */
  std::string added_by;
  std::string added_by_title;
  /** G020 6: how many on the roster they added, and how many they removed who are still out. */
  size_t added_count = 0;
  size_t removed_count = 0;

  bool IsContact() const { return !contact_id.empty(); }
  bool operator==(const GroupMemberRow&) const = default;
};

/**
 * `contact`: the saved contact for this member, when there is one. Name: the contact's, else
 * `card_nickname` (the member's own, from the account card the roster carried), else the short account id.
 */
GroupMemberRow BuildGroupMemberRow(const GroupRosterMember& member, const std::optional<Contact>& contact,
                                   bool unreachable, const std::string& card_nickname = {});
/** An invite waiting longer than this shows the "not joined yet" hint (it expires at kGroupInviteTtlMs). */
inline constexpr int64_t kInviteStaleAfterMs = 60LL * 60 * 1000;

/**
 * The row for someone this device invited who has not joined: nothing when they are already on the
 * roster (`roster_identities`), else a row marked `invited`, `invited_stale` after an hour. `contact` and
 * `card_nickname` name them as for a member.
 */
std::optional<GroupMemberRow> BuildInvitedRow(const PendingGroupInvite& invite,
                                              const std::unordered_set<std::string>& roster_identities,
                                              const std::optional<Contact>& contact, int64_t now_ms,
                                              const std::string& card_nickname = {});
/** Unreachable members first (the owner re-invites them), then the owner, then by name. */
void SortGroupMemberRows(std::vector<GroupMemberRow>& rows);
/**
 * G019 5 / G020 6 on the member rows: whose name "added by" shows (the adder's row title, else a short id; empty when
 * nobody is said to have added them), how many on `roster` each added, and (`removed_by`, may be empty) how many each
 * removed who are still out.
 */
void FillGroupAccountability(std::vector<GroupMemberRow>& rows, const std::vector<GroupRosterMember>& roster,
                             const std::function<size_t(const std::string& account)>& removed_by);

/**
 * One row of the "Group admin" card on a member's profile (phones) and the member menu (desktop). G020 6: remove
 * everyone they added (owner and admins), re-invite everyone they removed (owner).
 */
enum class GroupMemberAction { Reinvite, MakeAdmin, RemoveAdmin, Remove, RemoveEveryoneAdded, ReinviteEveryoneRemoved };

/** The grey note beside Re-invite: why the person is offered one. */
enum class GroupReinviteNote { None, NotJoined, Unreachable };

/** What a tap on someone in a group (member list, group info's member card, their avatar in the chat) does on a phone. */
struct GroupMemberTap {
  enum class Kind {
    /** Our own row. */
    Nothing,
    /** Their profile sheet, for everyone (Kenneth 2026-11-04: the owner too, no menu first). */
    OpenProfile,
  };
  Kind kind = Kind::Nothing;
  /** The owner's "Group admin" card at the bottom of that profile; empty for anyone else. */
  std::vector<GroupMemberAction> admin_actions;
  GroupReinviteNote reinvite_note = GroupReinviteNote::None;

  bool operator==(const GroupMemberTap&) const = default;
};
/**
 * The profile for anyone but ourselves. The owner's card: Re-invite (a contact still waiting, noted "not joined yet",
 * or out of reach, noted "can't be reached"), Make admin / Remove as admin (anyone on the roster, G017), Remove (anyone
 * on the roster). An admin's card: Remove, for members (G017, G020). Handing the group over is not here: it happens
 * when the owner leaves. `viewer_role`: ours (Owner when the group's recorded owner is us).
 */
GroupMemberTap DecideGroupMemberTap(const GroupMemberRow& row, bool is_self, MemberRole viewer_role,
                                    GroupPreset preset = GroupPreset::Open);
/** Whether we, at `viewer_role`, may remove `row` from the group (GroupAllows: the owner anyone, an admin members). */
bool MayRemoveGroupMember(const GroupMemberRow& row, MemberRole viewer_role, GroupPreset preset = GroupPreset::Open);

/** What the phone's group info sheet is built from (ChatPresenter::GroupInfoOf reads it; tests give it). */
struct GroupInfoFacts {
  std::string thread_id;
  std::string group_id;
  /** The name the chat shows (this device's own name for it when one is set). */
  std::string title;
  /** This device renamed the group for itself ("Restore group name" then shows). */
  bool has_local_title = false;
  bool dissolved = false;
  bool is_owner = false;
  bool muted = false;
  /** Our role (Owner when the group's recorded owner is us) and the group's preset (G017). */
  MemberRole viewer_role = MemberRole::Member;
  GroupPreset preset = GroupPreset::Open;
  /** Everyone on the roster, this device too (its row is `self_identity`'s, already titled "You"). */
  std::vector<GroupMemberRow> members;
  std::string self_identity;
  /** G021: the group's live invite links (GroupInviteLinkWorkflow::LiveLinks), and now (their expiry). */
  std::vector<GroupLinkRow> links;
  int64_t now_ms = 0;
};

/** The group info sheet (DEV/PLAN-ui-batch8-group-pages.md 1): which rows show, and the first members. */
struct GroupInfoScreen {
  std::string thread_id;
  std::string group_id;
  std::string title;
  bool dissolved = false;
  bool is_owner = false;
  bool muted = false;
  /** On the roster ("N people", "N members", "See all N members"). */
  int member_count = 0;
  /** The member card's first rows: the owner first, then as the member list sorts them. */
  std::vector<GroupMemberRow> preview;
  std::string self_identity;
  /** Hidden for a dissolved group (G016): members, calls, renaming, leaving. */
  bool show_members = false;
  bool show_calls = false;
  bool show_rename = false;
  /** Whoever the preset lets rename (G017) renames the group for everyone; anyone else names it for themselves. */
  bool rename_for_everyone = false;
  bool show_restore_name = false;
  /** Whoever the preset lets invite (G019). */
  bool show_add_member = false;
  /** "Who can add people and rename": Everyone (Open) / Admins only (Managed); the owner switches it (G017). */
  bool show_preset = false;
  bool preset_editable = false;
  GroupPreset preset = GroupPreset::Open;
  bool show_leave = false;
  bool show_dissolve = false;
  /** G021: "Invite link…" for whoever the preset lets make links; the live links listed for the owner and admins. */
  bool show_invite_link = false;
  bool show_links = false;
  std::vector<GroupLinkListRow> links;

  bool operator==(const GroupInfoScreen&) const = default;
};
/** The member card shows at most this many people; "See all" opens the whole list. */
inline constexpr size_t kGroupInfoPreviewMembers = 5;
GroupInfoScreen BuildGroupInfoScreen(const GroupInfoFacts& facts);

/** Visual chat type of a thread: ai | private | public | group. */
std::string ChatSessionKindName(const Thread& thread);

struct ThreadMessage;
/**
 * Whether this device's own reaction `emoji_key` is on `message_id` now: its latest reaction /
 * clear annotation for that emoji in `messages` (D098) is a reaction.
 */
bool MyReactionActive(const std::vector<ThreadMessage>& messages, const std::string& message_id,
                      const std::string& emoji_key);

/** What the user sees when a group action fails: a localized line, never the engine's English text. */
std::string GroupActionFailureText(const Error& error);

/**
 * The chats list's decisions and its buttons, without a GUI: which chats show and how (order,
 * unread, pin, mute, archive), and what opening one or changing its settings does. ChatController
 * renders the list (words, menus); pp-app-probe presses the same buttons and reports the same list,
 * so the app lab checks what people see. The pattern is CallPresenter's.
 *
 * UI thread.
 */
class ChatPresenter : public Module {
public:
  struct Callbacks {
    /** The list may have changed: re-read List(). */
    std::function<void()> on_changed;
    /** A settings write the store refused (the renderer tells the user). */
    std::function<void()> prefs_failed;
    /**
     * A message's text as prose, for the preview line: the app's (an AI reply is structured blocks,
     * CR007). None = the text as stored.
     */
    std::function<std::string(const std::string& text)> prose;
  };

  ChatPresenter();

  /** The messaging facade, or null while messaging is not ready (the list is then empty). */
  void BindFacade(ConversationsFacade* facade);
  void BindCallbacks(Callbacks callbacks);
  /** The profile's data was reset: nothing read from the old store may be written to the new one. */
  void ResetForNewProfile();

  /** Rebuild the list from the threads and their settings. */
  void Refresh();
  const ChatListScreen& List() const { return list_; }

  // --- buttons ----------------------------------------------------------------------------------

  /**
   * Open a chat: it is read (a "marked unread" dot goes too, and its mention marker), its notification is
   * cleared. OpenedAtMention() then names the oldest unread message that mentioned us there, if any.
   */
  bool SelectThread(const std::string& thread_id);
  /** The last SelectThread's chat had an unread mention: the message to show first (G015). */
  const std::optional<std::string>& OpenedAtMention() const { return opened_at_mention_; }
  /** Back to the chats list (the Home tab): no chat is open, so new messages count as unread. */
  void ShowHome();
  /** The archived list's entry / back button. */
  void ToggleArchived();
  /** The PP助手 page's own archived view (the Chats archive view is separate). */
  void ToggleArchivedAi();
  void SetPinned(const std::string& thread_id, bool pinned);
  void Mute(const std::string& thread_id, SessionMuteChoice choice);
  void Unmute(const std::string& thread_id);
  /** A muted group: its mentions silent too (G015), or alerting again. */
  void SetMuteMentions(const std::string& thread_id, bool mute);
  /** Undo "don't ask again" for sharing this chat with the AI: it asks again next time. */
  void AskBeforeSharingAi(const std::string& thread_id);
  /** The stored settings were changed by someone else (the facade): read them again before the next change. */
  void ForgetCachedPrefs();
  void SetArchived(const std::string& thread_id, bool archived);
  void MarkUnread(const std::string& thread_id);
  void MarkRead(const std::string& thread_id);
  /** A chat was closed / deleted: forget its settings. */
  void ForgetThread(const std::string& thread_id);

  // --- closing a chat ---------------------------------------------------------------------------

  CloseChatPlan PlanClose(const std::string& thread_id);
  /** Delete / close the chat here (and forget its settings). */
  Roe<void> CloseThread(const std::string& thread_id);
  /** Leave the group, then close the chat; a leave that cannot be sent dismisses the group here. */
  Roe<void> LeaveGroupAndClose(const std::string& thread_id, const std::string& group_id);
  Roe<void> DismissGroupAndClose(const std::string& thread_id, const std::string& group_id);
  /** G016: the owner dissolves the group for everyone. The chat stays (read-only), so nothing is closed. */
  Roe<void> DissolveGroup(const std::string& group_id);
  Roe<void> HandOverAndClose(const std::string& thread_id, const std::string& group_id, const std::string& successor);
  /** The owner's "choose a new owner" sheet for `thread_id`, read now (BuildOwnerLeaveSheet). */
  OwnerLeaveSheet OwnerLeaveSheetOf(const std::string& thread_id);
  /** `successor` can still take `thread_id`'s group over (asked again right before handing it over). */
  bool CanHandOverTo(const std::string& thread_id, const std::string& successor);

  /** One chat's settings (defaults when it has none); nullopt when the store cannot be read. */
  std::optional<ThreadLocalPrefs> PrefsOf(const std::string& thread_id);

  // --- the open chat's peer sheet (the header's person / group button) --------------------------

  /**
   * "Add to contacts" for the person of a direct chat who is not a contact yet: from the directory
   * entry this device holds for `peer_id` (else the id alone), with the keys and endpoints it carries;
   * the open direct chat is then bound to the new contact. The contact's id; an error's message is an
   * i18n key.
   */
  Roe<std::string> AddPeerToContacts(const std::string& peer_id);
  /**
   * Blocks someone who is not a saved contact. A block is a trust level on a contact record, so the
   * person is saved first, as a blocked contact (it shows in Security > Blocked, not as a friend).
   * Returns the contact id.
   */
  Roe<std::string> BlockStranger(const std::string& peer_id);
  ChatGroupSheet GroupSheetOf(const std::string& group_id);
  /** The phone's group info sheet for group chat `thread_id` (an empty screen when it is not one). */
  GroupInfoScreen GroupInfoOf(const std::string& thread_id);
  /**
   * The peer sheet's group actions. A failure's message is already the localized text to show (the engine's
   * own wording goes to the log only).
   */
  Roe<void> SetThreadLocalTitle(const std::string& thread_id, const std::string& local_title);
  Roe<void> RenameGroupShared(const std::string& group_id, const std::string& title);
  /**
   * G021, "Invite link…": our newest live link to the group whose text we keep, else a new one (7 days). Blocking:
   * a new link is signed and fanned out — call it off the UI thread.
   */
  Roe<GroupLinkRow> InviteLinkFor(const std::string& group_id);
  /** G021, "Revoke link": its maker's, or (the owner, admins) anyone's. Blocking. */
  Roe<void> RevokeInviteLink(const std::string& group_id, const std::string& link_id);
  Roe<void> RemoveGroupMember(const std::string& group_id, const std::string& member_identity);
  /** G017, the owner: who can add people and rename (Open / Managed). */
  Roe<void> SetGroupPreset(const std::string& group_id, GroupPreset preset);

  // --- a message's buttons (the open chat) ------------------------------------------------------

  /** *Add to contacts* on a contact card in the open chat: the contact's id (added, or already known). */
  Roe<std::string> AddContactFromCard(const std::string& message_id);
  /** *Share contact*: contact `contact_id` as a card into chat `thread_id`. */
  Roe<void> ShareContact(const std::string& thread_id, const std::string& contact_id);
  /** React with `emoji` on a message of the open chat, or take this device's reaction back. */
  Roe<void> ToggleReaction(const std::string& message_id, const std::string& emoji);
  Roe<void> DeleteForMe(const std::string& message_id);
  /** "Delete for everyone" is offered: our own message, in a chat with people (not the agent). */
  bool CanDeleteForEveryone(const std::string& message_id);
  /** Sends the delete (network: on a worker); `done` on the UI thread unless the presenter is gone. */
  void DeleteForEveryone(const std::string& message_id, std::function<void(Roe<void>)> done);

  // --- the composer (the open chat) -----------------------------------------------------------

  /** Reply quoting `quoted` (a message's text): the next send carries it under the reply. */
  void StartQuoteReply(const std::string& quoted);
  void CancelQuoteReply() { quote_source_.clear(); }
  bool QuotingReply() const { return !quote_source_.empty(); }
  const std::string& QuoteSource() const { return quote_source_; }
  /** The quote bar's one line: the quoted text, newlines flattened, cut at 120 characters with "…". */
  std::string QuotePreview() const;
  /** A draft that can be sent (not too long); the error's message is an i18n key. */
  static Roe<void> ValidateDraft(const std::string& text);
  /** The text to send for `reply`: with the quoted message under it while a quote reply is open. */
  std::string ComposeWithQuote(const std::string& reply) const;
  /** Send `text` into the open chat the way the composer does (the message router: a person, or the agent). */
  Roe<void> SendToChat(const std::string& text);
  /**
   * A group text with the mentions the composer marked (G015). False when it is not one: no spans, not a
   * group, or an "@ai" question (the agent's, sent the usual way) — then nothing was sent.
   */
  bool SendsWithMentions(const std::string& thread_id, const std::string& text, const std::vector<TextSpan>& spans);
  /** The text part of that rule: spans, and not an "@ai" question (a text that starts `@ai` is the agent's; its spans drop). */
  static bool KeepsMentionSpans(const std::string& text, const std::vector<TextSpan>& spans);
  /** Sends it (SendsWithMentions was true). */
  Roe<void> SendWithMentions(const std::string& thread_id, const std::string& text, std::vector<TextSpan> spans);

  // --- mentions in a group (G015) -------------------------------------------------------------

  /** What highlighting a group's mentions is checked against now; none for a chat that is not a group. */
  std::optional<MentionRoster> MentionRosterOf(const std::string& thread_id) const;
  /**
   * The composer's `@` picker for a group, unfiltered: its other members by public name (and `@everyone` for the
   * owner). Static and given the facade by value so a worker can run it holding nothing of the presenter; the
   * AppRuntime teardown gate drains workers before the messaging graph (and so the facade) goes.
   */
  static std::vector<MentionCandidate> ReadMentionCandidates(ConversationsFacade* facade, const std::string& thread_id);
  /** The messaging facade, for a worker that reads for the presenter (null before messaging is ready). */
  ConversationsFacade* Facade() const { return facade_; }
  /** `spans` without the mentions of anyone no longer on the group's roster (read now, at send). */
  std::vector<TextSpan> SpansOnRoster(const std::string& thread_id, std::vector<TextSpan> spans) const;
  /**
   * A tap on highlighted mention `run_index` of a message in the open group (EligibleMentionRuns' order): the
   * saved contact it names; none for `@everyone`, ourselves, or someone who is not a contact.
   */
  std::optional<std::string> MentionedContact(const std::string& message_id, size_t run_index);

private:
  /** A group's list preview: its latest membership line in the transcript's words, else the stored preview. */
  std::string GroupEventPreviewOr(const Thread& thread, const std::string& local_account);
  /**
   * Pin / mute / archive / marked-unread of every thread that has any set: read once when the store
   * can be listed, then kept in step with every write. Empty, and read again next time, while it
   * cannot.
   */
  const std::unordered_map<std::string, ThreadLocalPrefs>& AllPrefs();
  /** Writes the settings and the cache only; false when the store refused. */
  bool StorePrefs(const ThreadLocalPrefs& prefs);
  /** Reads, changes and writes one chat's settings, then refreshes; tells the renderer when refused. */
  void UpdatePrefs(const std::string& thread_id, const std::function<void(ThreadLocalPrefs&)>& change);
  void Changed();

  ConversationsFacade* facade_ = nullptr;
  Callbacks callbacks_;
  ChatListScreen list_;
  bool showing_archived_ = false;
  bool showing_archived_ai_ = false;
  std::optional<std::unordered_map<std::string, ThreadLocalPrefs>> prefs_;
  std::shared_ptr<int> alive_ = std::make_shared<int>(0);
  std::string quote_source_;
  std::optional<std::string> opened_at_mention_;
};

} // namespace pbr
