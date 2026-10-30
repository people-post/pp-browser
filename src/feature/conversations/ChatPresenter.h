#pragma once

#include "common/Error.h"
#include "common/Module.h"
#include "feature/conversations/SessionMenu.h"
#include "lib/messaging/GroupTypes.h"
#include "lib/messaging/ThreadLocalPrefsStore.h"
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
  /** Visual chat type: ai | private | public | group. */
  std::string kind;
  int unread_count = 0;
  /** Marked unread by hand with no real unread count: a dot, not a number. */
  bool unread_dot = false;
  bool pinned = false;
  bool muted = false;
  std::string date_label;
  bool active = false;

  bool operator==(const ChatSessionRow&) const = default;
};

/** The chats list: pinned first, then by activity; archived chats are a separate list. */
struct ChatListScreen {
  std::vector<ChatSessionRow> rows;
  int archived_count = 0;
  /** The archived list is showing (back to the main list once nothing is archived). */
  bool showing_archived = false;

  bool operator==(const ChatListScreen&) const = default;
};

/** What closing a chat does (the GUI asks before it does it). */
struct CloseChatPlan {
  enum class Kind {
    /** Delete the conversation (a person's; `DeleteAi`: the agent's). */
    Delete,
    DeleteAi,
    /** Leave the group (the owner stays). */
    Leave,
    /** Drop the group here only: an orphaned or solo group — no owner on the roster, or nobody reachable. */
    Dismiss,
    /** The owner leaves: hand the group to one of `successors` (reachable members), or dismiss it here. */
    LeaveAsOwner,
  };
  Kind kind = Kind::Delete;
  std::string group_id;
  std::vector<std::string> successors;
};
const char* CloseChatPlanKindName(CloseChatPlan::Kind kind);

/** A group's side of the open chat's peer sheet. */
struct ChatGroupSheet {
  /** This device owns the group. */
  bool is_owner = false;
  bool owner_unreachable = false;
  /** Members the owner may remove for being unreachable (never this device): identity, then the name to show. */
  std::vector<std::pair<std::string, std::string>> unreachable_members;
};

/** One person on the group's member list, before any text is chosen for the badges. */
struct GroupMemberRow {
  std::string identity;
  /** Contact name, else the server nickname, else the short account id. */
  std::string title;
  /** The saved contact's id; empty for someone who is not a contact yet. */
  std::string contact_id;
  bool is_owner = false;
  bool unreachable = false;
  /** Invited by this device, not on the roster yet (not a member; not counted in "N members"). */
  bool invited = false;
  /** Invited more than a day ago and still not joined. */
  bool invited_stale = false;

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
 * roster (`roster_identities`), else a row marked `invited`, `invited_stale` after a day. `contact` and
 * `card_nickname` name them as for a member.
 */
std::optional<GroupMemberRow> BuildInvitedRow(const PendingGroupInvite& invite,
                                              const std::unordered_set<std::string>& roster_identities,
                                              const std::optional<Contact>& contact, int64_t now_ms,
                                              const std::string& card_nickname = {});
/** Unreachable members first (the owner re-invites them), then the owner, then by name. */
void SortGroupMemberRows(std::vector<GroupMemberRow>& rows);

/** Visual chat type of a thread: ai | private | public | group. */
std::string ChatSessionKindName(const Thread& thread);

struct ThreadMessage;
/**
 * Whether this device's own reaction `emoji_key` is on `message_id` now: its latest reaction /
 * clear annotation for that emoji in `messages` (D098) is a reaction.
 */
bool MyReactionActive(const std::vector<ThreadMessage>& messages, const std::string& message_id,
                      const std::string& emoji_key);

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

  /** Open a chat: it is read (a "marked unread" dot goes too), its notification is cleared. */
  bool SelectThread(const std::string& thread_id);
  /** Back to the chats list (the Home tab): no chat is open, so new messages count as unread. */
  void ShowHome();
  /** The archived list's entry / back button. */
  void ToggleArchived();
  void SetPinned(const std::string& thread_id, bool pinned);
  void Mute(const std::string& thread_id, SessionMuteChoice choice);
  void Unmute(const std::string& thread_id);
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
  Roe<void> HandOverAndClose(const std::string& thread_id, const std::string& group_id, const std::string& successor);

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
  ChatGroupSheet GroupSheetOf(const std::string& group_id);

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

private:
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
  std::optional<std::unordered_map<std::string, ThreadLocalPrefs>> prefs_;
  std::shared_ptr<int> alive_ = std::make_shared<int>(0);
  std::string quote_source_;
};

} // namespace pbr
