#include "feature/conversations/ChatPresenter.h"

#include "feature/conversations/ConversationsFacade.h"
#include "foundation/i18n/LocalizationService.h"
#include "foundation/runtime/AppRuntime.h"
#include "lib/messaging/ChatPayloadCodec.h"
#include "lib/messaging/ChatPayloadValidator.h"
#include "lib/messaging/AtAiParser.h"
#include "lib/messaging/QuoteReply.h"
#include "lib/messaging/DeleteForEveryone.h"
#include "lib/messaging/ReactionTypes.h"
#include "common/EmojiKey.h"
#include "feature/conversations/SessionListText.h"
#include "foundation/platform/ILocalNotifier.h"
#include "lib/ai/StructuredTextParser.h"
#include "common/Utilities.h"
#include "lib/messaging/GroupTypes.h"
#include "lib/people/PeerDisplayLabel.h"

#include <algorithm>
#include <set>
#include <utility>

namespace pbr {

std::string ChatSessionKindName(const Thread& thread) {
  switch (thread.kind) {
  case ThreadKind::Ai:
    return "ai";
  case ThreadKind::Group:
    return "group";
  case ThreadKind::Direct:
    return thread.channel == ThreadChannel::E2e ? "private" : "public";
  }
  return "public";
}

GroupMemberRow BuildGroupMemberRow(const GroupRosterMember& member, const std::optional<Contact>& contact,
                                   const bool unreachable, const std::string& card_nickname) {
  GroupMemberRow row;
  row.identity = member.member_identity;
  row.is_owner = member.role == MemberRole::Owner;
  row.unreachable = unreachable;
  if (contact) {
    row.contact_id = contact->id;
    row.title = FormatContactTitle(*contact);
  }
  if (row.title.empty()) {
    row.title = card_nickname;
  }
  if (row.title.empty()) {
    row.title = ShortRelayId(member.member_identity);
  }
  return row;
}

std::optional<GroupMemberRow> BuildInvitedRow(const PendingGroupInvite& invite,
                                              const std::unordered_set<std::string>& roster_identities,
                                              const std::optional<Contact>& contact, const int64_t now_ms,
                                              const std::string& card_nickname) {
  if (roster_identities.count(invite.invitee_identity) > 0) {
    return std::nullopt;
  }
  GroupRosterMember as_member;
  as_member.member_identity = invite.invitee_identity;
  GroupMemberRow row = BuildGroupMemberRow(as_member, contact, false, card_nickname);
  row.invited = true;
  row.invited_stale = now_ms - invite.created_at > kInviteStaleAfterMs;
  return row;
}

void SortGroupMemberRows(std::vector<GroupMemberRow>& rows) {
  std::stable_sort(rows.begin(), rows.end(), [](const GroupMemberRow& a, const GroupMemberRow& b) {
    if (a.unreachable != b.unreachable) {
      return a.unreachable;
    }
    if (a.is_owner != b.is_owner) {
      return a.is_owner;
    }
    return a.title < b.title;
  });
}

bool MyReactionActive(const std::vector<ThreadMessage>& messages, const std::string& message_id,
                      const std::string& emoji_key) {
  int64_t latest_order = -1;
  bool active = false;
  for (const ThreadMessage& message : messages) {
    if (message.content_type != ChatContentType::Annotation || message.sender_contact_id != kLocalSelfContactId ||
        !message.target_message_id || *message.target_message_id != message_id) {
      continue;
    }
    std::string annotation_type = kAnnotationTypeReaction;
    std::string value = message.text;
    if (auto fields = ChatPayloadCodec::DecodeAnnotationJson(message.payload_json)) {
      annotation_type = fields->annotation_type;
      value = fields->value.empty() ? message.text : fields->value;
    }
    if (!IsReactionAnnotationType(annotation_type) || NormalizeEmojiKey(value) != emoji_key) {
      continue;
    }
    if (message.display_order >= latest_order) {
      latest_order = message.display_order;
      active = annotation_type == kAnnotationTypeReaction;
    }
  }
  return active;
}

const char* CloseChatPlanKindName(const CloseChatPlan::Kind kind) {
  switch (kind) {
  case CloseChatPlan::Kind::Delete:
    return "delete";
  case CloseChatPlan::Kind::DeleteAi:
    return "delete_ai";
  case CloseChatPlan::Kind::Leave:
    return "leave";
  case CloseChatPlan::Kind::Dismiss:
    return "dismiss";
  case CloseChatPlan::Kind::LeaveAsOwner:
    return "leave_as_owner";
  }
  return "?";
}

ChatPresenter::ChatPresenter() {
  redirectLogger("ChatPresenter");
}

void ChatPresenter::BindFacade(ConversationsFacade* facade) {
  facade_ = facade;
  prefs_.reset();
}

void ChatPresenter::BindCallbacks(Callbacks callbacks) {
  callbacks_ = std::move(callbacks);
}

void ChatPresenter::ResetForNewProfile() {
  prefs_.reset();
  showing_archived_ = false;
}

const std::unordered_map<std::string, ThreadLocalPrefs>& ChatPresenter::AllPrefs() {
  static const std::unordered_map<std::string, ThreadLocalPrefs> kUnavailable;
  if (!prefs_ && facade_) {
    auto listed = facade_->ListThreadLocalPrefs();
    if (!listed) {
      // Not cached: an empty cache would make every thread look default, and the next write would put
      // those defaults over the thread's stored row. Read again next time instead.
      return kUnavailable;
    }
    prefs_.emplace();
    for (ThreadLocalPrefs& prefs : *listed) {
      const std::string id = prefs.thread_id;
      prefs_->emplace(id, std::move(prefs));
    }
  }
  return prefs_ ? *prefs_ : kUnavailable;
}

std::optional<ThreadLocalPrefs> ChatPresenter::PrefsOf(const std::string& thread_id) {
  if (!facade_) {
    return std::nullopt;
  }
  const auto& all = AllPrefs();
  if (!prefs_) {
    auto one = facade_->GetThreadLocalPrefs(thread_id);
    if (!one) {
      return std::nullopt;
    }
    return *one;
  }
  if (const auto it = all.find(thread_id); it != all.end()) {
    return it->second;
  }
  ThreadLocalPrefs prefs;
  prefs.thread_id = thread_id;
  return prefs;
}

bool ChatPresenter::StorePrefs(const ThreadLocalPrefs& prefs) {
  if (!facade_ || !facade_->SetThreadLocalPrefs(prefs)) {
    return false;
  }
  if (prefs_) {
    (*prefs_)[prefs.thread_id] = prefs;
  }
  return true;
}

void ChatPresenter::UpdatePrefs(const std::string& thread_id, const std::function<void(ThreadLocalPrefs&)>& change) {
  std::optional<ThreadLocalPrefs> prefs = PrefsOf(thread_id);
  if (prefs) {
    change(*prefs);
  }
  if (!prefs || !StorePrefs(*prefs)) {
    if (callbacks_.prefs_failed) {
      callbacks_.prefs_failed();
    }
    Changed();
    return;
  }
  Refresh();
}

void ChatPresenter::Refresh() {
  ChatListScreen next;
  next.showing_archived = showing_archived_;
  auto threads = facade_ ? facade_->ListThreads() : Roe<std::vector<Thread>>(Error("Messaging not ready"));
  if (!threads) {
    list_ = std::move(next);
    Changed();
    return;
  }
  const auto& prefs = AllPrefs();
  const auto prefs_of = [&prefs](const Thread& thread) -> ThreadLocalPrefs {
    const auto it = prefs.find(thread.id);
    return it == prefs.end() ? ThreadLocalPrefs{} : it->second;
  };
  std::vector<Thread> sorted = std::move(*threads);
  std::stable_sort(sorted.begin(), sorted.end(), [&prefs_of](const Thread& a, const Thread& b) {
    return SessionSortsBefore(prefs_of(a).pinned_at, a.updated_at, prefs_of(b).pinned_at, b.updated_at);
  });
  for (const Thread& thread : sorted) {
    if (prefs_of(thread).archived) {
      ++next.archived_count;
    }
  }
  if (next.archived_count == 0) {
    showing_archived_ = false;  // the last archived chat left: back to the list
    next.showing_archived = false;
  }
  const std::string active_id = facade_->ActiveThreadId();
  const int64_t now_ms = util::NowUnixMs();
  const std::set<std::string> mentioned = facade_->ThreadsWithMentions();
  for (const Thread& thread : sorted) {
    const ThreadLocalPrefs thread_prefs = prefs_of(thread);
    if (thread_prefs.archived != next.showing_archived) {
      continue;
    }
    ChatSessionRow row;
    row.id = thread.id;
    row.title = facade_->ResolveThreadLabel(thread).title;
    // The stored default AI title is English; the renderer shows it in the UI language.
    row.title_is_default_ai = thread.kind == ThreadKind::Ai && row.title == "New chat";
    row.preview = SessionPreviewLine(StructuredTextParser::PlainText(thread.preview));
    row.kind = ChatSessionKindName(thread);
    row.unread_count = thread.unread_count;
    row.unread_dot = SessionShowsUnreadDot(thread.unread_count, thread_prefs.marked_unread);
    row.mentioned = mentioned.count(thread.id) > 0;
    row.pinned = thread_prefs.pinned_at != 0;
    row.muted = ThreadIsMuted(thread_prefs.muted_until, now_ms);
    row.date_label = SessionDateLabel(thread.updated_at, now_ms);
    row.active = thread.id == active_id;
    next.rows.push_back(std::move(row));
  }
  list_ = std::move(next);
  Changed();
}

void ChatPresenter::Changed() {
  if (callbacks_.on_changed) {
    callbacks_.on_changed();
  }
}

bool ChatPresenter::SelectThread(const std::string& thread_id) {
  opened_at_mention_.reset();
  if (!facade_) {
    return false;
  }
  // Read before opening: opening reads the chat, which drops its mention marker.
  std::optional<std::string> mention = facade_->OldestMention(thread_id);
  if (!facade_->OpenThread(thread_id)) {
    return false;
  }
  opened_at_mention_ = std::move(mention);
  // Opening a chat reads it: a dot set by "Mark as unread" goes with the unread count.
  if (std::optional<ThreadLocalPrefs> prefs = PrefsOf(thread_id); prefs && prefs->marked_unread) {
    prefs->marked_unread = false;
    (void)StorePrefs(*prefs);
  }
  ILocalNotifier::Instance().ClearForThread(thread_id);
  facade_->MaybeTailSync(thread_id);
  return true;
}

void ChatPresenter::ShowHome() {
  if (facade_) {
    facade_->ClearActiveThread();
  }
  Refresh();
}

void ChatPresenter::ToggleArchived() {
  showing_archived_ = !showing_archived_;
  Refresh();
}

void ChatPresenter::SetPinned(const std::string& thread_id, const bool pinned) {
  UpdatePrefs(thread_id, [pinned](ThreadLocalPrefs& prefs) { prefs.pinned_at = pinned ? util::NowUnixMs() : 0; });
}

void ChatPresenter::Mute(const std::string& thread_id, const SessionMuteChoice choice) {
  UpdatePrefs(thread_id,
              [choice](ThreadLocalPrefs& prefs) { prefs.muted_until = MutedUntilFor(choice, util::NowUnixMs()); });
}

void ChatPresenter::Unmute(const std::string& thread_id) {
  UpdatePrefs(thread_id, [](ThreadLocalPrefs& prefs) {
    prefs.muted_until = 0;
    prefs.mute_mentions = false;  // a mute's own setting: the next mute starts with mentions alerting
  });
}

void ChatPresenter::SetMuteMentions(const std::string& thread_id, const bool mute) {
  UpdatePrefs(thread_id, [mute](ThreadLocalPrefs& prefs) { prefs.mute_mentions = mute; });
}

void ChatPresenter::SetArchived(const std::string& thread_id, const bool archived) {
  UpdatePrefs(thread_id, [archived](ThreadLocalPrefs& prefs) { prefs.archived = archived; });
}

void ChatPresenter::MarkUnread(const std::string& thread_id) {
  // A mark, not a message count: the row shows a dot and keeps its place in the list.
  UpdatePrefs(thread_id, [](ThreadLocalPrefs& prefs) { prefs.marked_unread = true; });
}

void ChatPresenter::MarkRead(const std::string& thread_id) {
  if (facade_) {
    facade_->MarkThreadRead(thread_id);
  }
  UpdatePrefs(thread_id, [](ThreadLocalPrefs& prefs) { prefs.marked_unread = false; });
}

Roe<std::string> ChatPresenter::AddPeerToContacts(const std::string& peer_id) {
  if (!facade_ || peer_id.empty()) {
    return Error("chat.error.add_contact_failed");
  }
  mesh::DirectoryHit hit;
  if (auto shadow = facade_->GetDirectoryShadow(peer_id)) {
    hit = *shadow;
  } else {
    hit.hit_id = peer_id;
    if (peer_id.rfind("account:", 0) == 0) {
      hit.account_id = peer_id;
      hit.ids = {{mesh::ContactIdKind::Account, peer_id, true}};
    } else {
      hit.ids = {{mesh::ContactIdKind::RelayUser, peer_id, false}};
    }
  }
  auto created = facade_->AddContactFromDirectoryHit(hit);
  if (!created) {
    return Error("chat.error.add_contact_failed");
  }
  if (hit.nickname.empty()) {
    facade_->EnsureDirectoryLookup(peer_id); // the hit fills the nickname in when it arrives
  }
  const bool carries_keys = (hit.signing_public_key_b64 && !hit.signing_public_key_b64->empty()) ||
                            (hit.kem_public_key_b64 && !hit.kem_public_key_b64->empty());
  if (carries_keys && (!hit.account_id || hit.account_id->empty())) {
    return Error("chat.error.directory_missing_account");
  }
  facade_->RegisterPeerKeysFromDirectory(hit);
  facade_->RegisterContactDirectEndpoints(*created);
  // The direct chat with a stranger (no participants) belongs to the new contact from now on.
  ThreadChannel channel = ThreadChannel::E2ePublic;
  if (auto active = facade_->GetActiveThread(); active && active->kind == ThreadKind::Direct) {
    channel = active->channel;
  }
  (void)facade_->FindOrCreateDirectThread(created->id, channel);
  facade_->NotifyThreadChanged();
  return created->id;
}

ChatGroupSheet ChatPresenter::GroupSheetOf(const std::string& group_id) {
  ChatGroupSheet sheet;
  if (!facade_) {
    return sheet;
  }
  std::string local_identity;
  if (auto identity = facade_->GetIdentity()) {
    local_identity = identity->account_id;
    if (auto roster = facade_->ListGroupRoster(group_id)) {
      sheet.is_owner = std::any_of(roster->begin(), roster->end(), [&](const GroupRosterMember& member) {
        return member.member_identity == local_identity && member.role == MemberRole::Owner;
      });
    }
  }
  sheet.owner_unreachable = facade_->IsOwnerUnreachable(group_id);
  if (!sheet.is_owner) {
    return sheet;
  }
  for (const std::string& identity : facade_->ListUnreachableMembers(group_id)) {
    if (identity == local_identity) {
      continue;
    }
    std::string name;
    if (auto contact = facade_->FindContactByIdentity(identity, mesh::ContactIdKind::Account); contact && *contact) {
      name = (*contact)->display_name.empty() ? (*contact)->server_nickname : (*contact)->display_name;
    }
    sheet.unreachable_members.emplace_back(identity, name.empty() ? identity : name);
  }
  return sheet;
}

Roe<std::string> ChatPresenter::AddContactFromCard(const std::string& message_id) {
  const std::string thread_id = facade_ ? facade_->ActiveThreadId() : std::string();
  if (thread_id.empty() || message_id.empty()) {
    return Error("No contact card to add");
  }
  auto contact = facade_->AddContactFromCard(thread_id, message_id);
  if (!contact) {
    return contact.error();
  }
  return contact->id;
}

Roe<void> ChatPresenter::ShareContact(const std::string& thread_id, const std::string& contact_id) {
  if (!facade_ || thread_id.empty() || contact_id.empty()) {
    return Error("Nothing to share");
  }
  if (auto sent = facade_->ShareContact(thread_id, contact_id); !sent) {
    return sent.error();
  }
  return {};
}

Roe<void> ChatPresenter::ToggleReaction(const std::string& message_id, const std::string& emoji) {
  const std::string thread_id = facade_ ? facade_->ActiveThreadId() : std::string();
  const std::string key = NormalizeEmojiKey(emoji);
  if (thread_id.empty() || message_id.empty() || key.empty()) {
    return Error("No message to react to");
  }
  bool mine_active = false;
  if (auto page = facade_->GetMessagesPage(thread_id, std::nullopt, 500)) {
    mine_active = MyReactionActive(*page, message_id, key);
  }
  auto sent = mine_active ? facade_->ClearReaction(thread_id, message_id, key)
                          : facade_->SendReaction(thread_id, message_id, key);
  if (!sent) {
    return sent.error();
  }
  return {};
}

Roe<void> ChatPresenter::DeleteForMe(const std::string& message_id) {
  const std::string thread_id = facade_ ? facade_->ActiveThreadId() : std::string();
  if (thread_id.empty() || message_id.empty()) {
    return Error("No message to delete");
  }
  return facade_->DeleteMessageForMe(thread_id, message_id);
}

bool ChatPresenter::CanDeleteForEveryone(const std::string& message_id) {
  if (!facade_ || message_id.empty()) {
    return false;
  }
  auto thread = facade_->GetActiveThread();
  if (!thread || thread->kind == ThreadKind::Ai) {
    return false;
  }
  auto found = facade_->GetMessage(thread->id, message_id);
  return found && found->has_value() && pbr::CanDeleteForEveryone(**found);
}

void ChatPresenter::DeleteForEveryone(const std::string& message_id, std::function<void(Roe<void>)> done) {
  ConversationsFacade* facade = facade_;
  const std::string thread_id = facade ? facade->ActiveThreadId() : std::string();
  if (thread_id.empty() || message_id.empty()) {
    if (done) {
      done(Error("No message to delete"));
    }
    return;
  }
  // Sends a message and may call the relay: off the UI thread, answer from the reply.
  AppRuntime::PostWorkerAndReplyOnUI<Roe<void>>(
      WorkerLane::Normal,
      [facade, thread_id, message_id]() { return facade->DeleteMessageForEveryone(thread_id, message_id); },
      [alive = std::weak_ptr<int>(alive_), done = std::move(done)](Roe<void> result) {
        if (alive.lock() && done) {
          done(std::move(result));
        }
      });
}

void ChatPresenter::StartQuoteReply(const std::string& quoted) {
  quote_source_ = quoted;
}

std::string ChatPresenter::QuotePreview() const {
  constexpr size_t kMaxPreviewChars = 120;
  std::string preview;
  size_t chars = 0;
  for (const char ch : quote_source_) {
    if ((static_cast<unsigned char>(ch) & 0xC0) != 0x80 && ++chars > kMaxPreviewChars) {
      preview += "\xE2\x80\xA6";  // …
      break;
    }
    preview += (ch == '\n' || ch == '\r') ? ' ' : ch;
  }
  return preview;
}

Roe<void> ChatPresenter::ValidateDraft(const std::string& text) {
  if (auto valid = ChatPayloadValidator::ValidateOutboundText(text); !valid) {
    return Error("chat.error.message_too_long");
  }
  return {};
}

std::string ChatPresenter::ComposeWithQuote(const std::string& reply) const {
  return quote_source_.empty() ? reply : ComposeQuoteReply(reply, quote_source_);
}

Roe<void> ChatPresenter::SendToChat(const std::string& text) {
  const std::string thread_id = facade_ ? facade_->ActiveThreadId() : std::string();
  if (thread_id.empty()) {
    return Error("No chat is open");
  }
  return facade_->RouteMessage(thread_id, text, std::nullopt);
}

namespace {

/** The group a chat is, if it is one. */
std::optional<std::string> GroupIdOf(ConversationsFacade* facade, const std::string& thread_id) {
  auto thread = facade ? facade->GetThread(thread_id) : Roe<std::optional<Thread>>(std::optional<Thread>{});
  if (!thread || !*thread || (*thread)->kind != ThreadKind::Group || !(*thread)->group_id) {
    return std::nullopt;
  }
  return *(*thread)->group_id;
}

} // namespace

bool ChatPresenter::SendsWithMentions(const std::string& thread_id, const std::string& text,
                                      const std::vector<TextSpan>& spans) {
  return KeepsMentionSpans(text, spans) && GroupIdOf(facade_, thread_id);
}

bool ChatPresenter::KeepsMentionSpans(const std::string& text, const std::vector<TextSpan>& spans) {
  return !spans.empty() && !ParseAtAiPrefix(text).is_ai_invoke;
}

Roe<void> ChatPresenter::SendWithMentions(const std::string& thread_id, const std::string& text,
                                          std::vector<TextSpan> spans) {
  if (!facade_) {
    return Error("Messaging not ready");
  }
  auto sent = facade_->SendGroupText(thread_id, text, std::move(spans));
  if (!sent) {
    return sent.error();
  }
  return {};
}

std::optional<MentionRoster> ChatPresenter::MentionRosterOf(const std::string& thread_id) const {
  const std::optional<std::string> group_id = GroupIdOf(facade_, thread_id);
  if (!group_id) {
    return std::nullopt;
  }
  MentionRoster roster;
  if (auto members = facade_->ListGroupRoster(*group_id)) {
    for (const GroupRosterMember& member : *members) {
      roster.members.insert(member.member_identity);
    }
  }
  if (auto owner = facade_->OwnerIdentity(*group_id)) {
    roster.owner = *owner;
  }
  if (auto identity = facade_->GetIdentity()) {
    roster.local_account = identity->account_id;
  }
  return roster;
}

std::vector<MentionCandidate> ChatPresenter::MentionCandidatesFor(const std::string& thread_id) {
  const std::optional<std::string> group_id = GroupIdOf(facade_, thread_id);
  if (!group_id) {
    return {};
  }
  auto members = facade_->ListGroupRoster(*group_id);
  const std::optional<MentionRoster> roster = MentionRosterOf(thread_id);
  if (!members || !roster || roster->local_account.empty()) {
    return {};
  }
  ConversationsFacade* facade = facade_;
  const auto public_name = [facade](const std::string& account_id) { return facade->MemberPublicNameOf(account_id); };
  // `@everyone` is the owner's: the one the roster names (G012), whatever role a stale row says.
  const bool local_is_owner = roster->owner == roster->local_account;
  return AllMentionCandidates(*members, roster->local_account, local_is_owner, public_name,
                              Tr("chat.mention.everyone_word"));
}

std::optional<std::string> ChatPresenter::MentionedContact(const std::string& message_id, const size_t run_index) {
  const std::string thread_id = facade_ ? facade_->ActiveThreadId() : std::string();
  const std::optional<MentionRoster> roster = thread_id.empty() ? std::nullopt : MentionRosterOf(thread_id);
  if (!roster) {
    return std::nullopt;
  }
  auto message = facade_->GetMessage(thread_id, message_id);
  if (!message || !*message) {
    return std::nullopt;
  }
  const ThreadMessage& shown = **message;
  const std::string sender =
      shown.sender_contact_id == kLocalSelfContactId ? roster->local_account : shown.sender_contact_id;
  const std::vector<MentionRun> runs = EligibleMentionRuns(
      shown.text, MentionTextLimit(shown.text), text_spans::FromPayloadJson(shown.payload_json), *roster, sender);
  if (run_index >= runs.size() || runs[run_index].kind != TextSpanKind::Mention ||
      runs[run_index].account_id == roster->local_account) {
    return std::nullopt;
  }
  auto contact = facade_->FindContactByIdentity(runs[run_index].account_id, mesh::ContactIdKind::Account);
  if (!contact || !*contact) {
    return std::nullopt;
  }
  return (*contact)->id;
}

CloseChatPlan ChatPresenter::PlanClose(const std::string& thread_id) {
  CloseChatPlan plan;
  auto thread = facade_ ? facade_->GetThread(thread_id) : Roe<std::optional<Thread>>(std::optional<Thread>{});
  if (!thread || !*thread) {
    return plan;
  }
  if ((*thread)->kind == ThreadKind::Ai) {
    plan.kind = CloseChatPlan::Kind::DeleteAi;
    return plan;
  }
  if ((*thread)->kind != ThreadKind::Group || !(*thread)->group_id) {
    return plan;
  }
  plan.group_id = *(*thread)->group_id;
  std::string me;
  if (auto identity = facade_->GetIdentity()) {
    me = identity->account_id;
  }
  std::vector<GroupRosterMember> members;
  if (auto roster = facade_->ListGroupRoster(plan.group_id)) {
    members = *roster;
  }
  bool is_owner = false;
  if (auto owner = facade_->IsLocalOwner(plan.group_id)) {
    is_owner = *owner;
  } else {
    is_owner = std::any_of(members.begin(), members.end(), [&](const GroupRosterMember& member) {
      return member.member_identity == me && member.role == MemberRole::Owner;
    });
  }
  for (const GroupRosterMember& member : members) {
    if (member.member_identity != me && !facade_->IsMemberUnreachable(plan.group_id, member.member_identity)) {
      plan.successors.push_back(member.member_identity);
    }
  }
  if (is_owner) {
    // Solo, or only unreachable peers: no transfer into a ghost roster.
    plan.kind = plan.successors.empty() ? CloseChatPlan::Kind::Dismiss : CloseChatPlan::Kind::LeaveAsOwner;
    return plan;
  }
  bool owner_on_roster = false;
  if (auto owner_id = facade_->OwnerIdentity(plan.group_id)) {
    owner_on_roster = std::any_of(members.begin(), members.end(),
                                  [&](const GroupRosterMember& member) { return member.member_identity == *owner_id; });
  }
  // Not the owner: leave — unless the group is an orphaned or solo shell (no owner on the roster, or
  // nobody reachable): then drop it here. Covers "both sides solo, neither is owner".
  plan.kind = plan.successors.empty() || !owner_on_roster ? CloseChatPlan::Kind::Dismiss : CloseChatPlan::Kind::Leave;
  if (plan.kind != CloseChatPlan::Kind::LeaveAsOwner) {
    plan.successors.clear();
  }
  return plan;
}

Roe<void> ChatPresenter::CloseThread(const std::string& thread_id) {
  if (!facade_) {
    return Error("Messaging not ready");
  }
  if (auto closed = facade_->CloseThread(thread_id); !closed) {
    return closed.error();
  }
  ForgetThread(thread_id);
  Refresh();
  return {};
}

Roe<void> ChatPresenter::LeaveGroupAndClose(const std::string& thread_id, const std::string& group_id) {
  if (!facade_) {
    return Error("Messaging not ready");
  }
  if (auto left = facade_->LeaveGroup(group_id); !left) {
    return DismissGroupAndClose(thread_id, group_id);
  }
  return CloseThread(thread_id);
}

Roe<void> ChatPresenter::DismissGroupAndClose(const std::string& thread_id, const std::string& group_id) {
  if (!facade_) {
    return Error("Messaging not ready");
  }
  (void)facade_->DismissLocalGroup(group_id);
  return CloseThread(thread_id);
}

Roe<void> ChatPresenter::HandOverAndClose(const std::string& thread_id, const std::string& group_id,
                                          const std::string& successor) {
  if (!facade_) {
    return Error("Messaging not ready");
  }
  if (auto left = facade_->LeaveAsOwner(group_id, successor); !left) {
    return left.error();
  }
  return CloseThread(thread_id);
}

void ChatPresenter::ForgetThread(const std::string& thread_id) {
  if (prefs_) {
    prefs_->erase(thread_id);
  }
}

} // namespace pbr
