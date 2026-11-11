#include "feature/presenters/ChatPresenter.h"

#include "lib/messaging/GroupPermissions.h"

#include "feature/conversations/ConversationsFacade.h"
#include "common/runtime/AppRuntime.h"
#include "common/PbrCompat.h"
#include "feature/presenters/DeliveryNoticePlan.h"
#include "feature/presenters/GroupEventText.h"
#include "feature/presenters/HomeToday.h"
#include "foundation/i18n/LocalizationService.h"
#include "lib/delivery/ChatPayloadCodec.h"
#include "lib/delivery/ChatPayloadValidator.h"
#include "lib/messaging/AtAiParser.h"
#include "lib/messaging/QuoteReply.h"
#include "lib/messaging/DeleteForEveryone.h"
#include "lib/delivery/ReactionTypes.h"
#include "common/EmojiKey.h"
#include "feature/presenters/SessionListText.h"
#include "foundation/platform/ILocalNotifier.h"
#include "common/Utilities.h"
#include "lib/messaging/GroupRosterSync.h"
#include "lib/messaging/GroupTypes.h"
#include "lib/people/AvatarGlyph.h"
#include "lib/people/ContactIdentity.h"
#include "lib/people/PeerDisplayLabel.h"

#include <algorithm>
#include <map>
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
  row.stable_id = contact ? ContactAvatarStableId(*contact) : member.member_identity;
  row.is_owner = member.role == MemberRole::Owner;
  row.is_admin = member.role == MemberRole::Admin;
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
    if (a.is_admin != b.is_admin) {
      return a.is_admin; // those who keep order next (G017)
    }
    return a.title < b.title;
  });
}

void FillGroupAccountability(std::vector<GroupMemberRow>& rows, const std::vector<GroupRosterMember>& roster,
                             const std::function<size_t(const std::string& account)>& removed_by) {
  std::unordered_map<std::string, size_t> added;
  std::unordered_map<std::string, std::string> adder_of;
  for (const GroupRosterMember& m : roster) {
    if (!m.added_by.empty() && m.added_by != m.member_identity) {
      ++added[m.added_by];
      adder_of[m.member_identity] = m.added_by;
    }
  }
  std::unordered_map<std::string, std::string> titles;
  for (const GroupMemberRow& row : rows) {
    titles[row.identity] = row.title;
  }
  for (GroupMemberRow& row : rows) {
    if (auto adder = adder_of.find(row.identity); adder != adder_of.end()) {
      row.added_by = adder->second;
      auto title = titles.find(adder->second);
      row.added_by_title = title != titles.end() ? title->second : ShortRelayId(adder->second);
    }
    if (auto count = added.find(row.identity); count != added.end()) {
      row.added_count = count->second;
    }
    row.removed_count = removed_by && !row.invited ? removed_by(row.identity) : 0;
  }
}

bool MayRemoveGroupMember(const GroupMemberRow& row, const MemberRole viewer_role, const GroupPreset preset) {
  const MemberRole target = row.is_owner ? MemberRole::Owner : row.is_admin ? MemberRole::Admin : MemberRole::Member;
  return !row.invited && GroupAllows(GroupAction::Remove, viewer_role, preset, target);
}

namespace {

/** G020 6: the bulk remedies for the person `row` is — remove whom they added, re-invite whom they removed. */
void AddBulkActions(GroupMemberTap& tap, const GroupMemberRow& row, const MemberRole viewer_role,
                    const GroupPreset preset) {
  if (row.invited) {
    return;
  }
  if (row.added_count > 0 && GroupAllows(GroupAction::Remove, viewer_role, preset, MemberRole::Member)) {
    tap.admin_actions.push_back(GroupMemberAction::RemoveEveryoneAdded);
  }
  if (row.removed_count > 0 && viewer_role == MemberRole::Owner) {
    tap.admin_actions.push_back(GroupMemberAction::ReinviteEveryoneRemoved);
  }
}

} // namespace

GroupMemberTap DecideGroupMemberTap(const GroupMemberRow& row, const bool is_self, const MemberRole viewer_role,
                                    const GroupPreset preset) {
  GroupMemberTap tap;
  if (is_self) {
    return tap;
  }
  tap.kind = GroupMemberTap::Kind::OpenProfile;
  if (viewer_role != MemberRole::Owner) {
    if (MayRemoveGroupMember(row, viewer_role, preset)) {
      tap.admin_actions.push_back(GroupMemberAction::Remove); // an admin removes members (G020)
    }
    AddBulkActions(tap, row, viewer_role, preset);
    return tap;
  }
  // A member already in the group needs no invite; resend only to someone still waiting or out of reach.
  if (row.IsContact() && (row.invited || row.unreachable)) {
    tap.admin_actions.push_back(GroupMemberAction::Reinvite);
    tap.reinvite_note = row.invited ? GroupReinviteNote::NotJoined : GroupReinviteNote::Unreachable;
  }
  if (!row.invited) {
    tap.admin_actions.push_back(row.is_admin ? GroupMemberAction::RemoveAdmin : GroupMemberAction::MakeAdmin);
    tap.admin_actions.push_back(GroupMemberAction::Remove);
  }
  AddBulkActions(tap, row, viewer_role, preset);
  return tap;
}

GroupInfoScreen BuildGroupInfoScreen(const GroupInfoFacts& facts) {
  GroupInfoScreen screen;
  screen.thread_id = facts.thread_id;
  screen.group_id = facts.group_id;
  screen.title = facts.title;
  screen.dissolved = facts.dissolved;
  screen.muted = facts.muted;
  screen.self_identity = facts.self_identity;
  if (facts.dissolved) {
    return screen; // G016: the name, "Group dissolved" and mute; nobody left to list, call or leave
  }
  screen.is_owner = facts.is_owner;
  screen.member_count = static_cast<int>(facts.members.size());
  std::vector<GroupMemberRow> rows = facts.members;
  SortGroupMemberRows(rows);
  // The owner heads the card (the full list keeps the unreachable first, for the owner to re-invite).
  std::stable_partition(rows.begin(), rows.end(), [](const GroupMemberRow& row) { return row.is_owner; });
  if (rows.size() > kGroupInfoPreviewMembers) {
    rows.resize(kGroupInfoPreviewMembers);
  }
  screen.preview = std::move(rows);
  screen.show_members = true;
  screen.show_calls = true;
  screen.show_rename = true;
  const MemberRole role = facts.is_owner ? MemberRole::Owner : facts.viewer_role;
  screen.rename_for_everyone = GroupAllows(GroupAction::Rename, role, facts.preset);
  screen.show_restore_name = facts.has_local_title;
  screen.show_add_member = GroupAllows(GroupAction::Invite, role, facts.preset);
  screen.show_preset = true;
  screen.preset_editable = GroupAllows(GroupAction::SetPreset, role, facts.preset);
  screen.preset = facts.preset;
  screen.show_leave = true;
  screen.show_dissolve = facts.is_owner;
  screen.show_invite_link = ShowsInviteLink(role, facts.preset);
  if (ListsGroupLinks(role, facts.preset) && !facts.links.empty()) {
    std::map<std::string, std::string> names;
    for (const GroupMemberRow& member : facts.members) {
      names[member.identity] = member.title;
    }
    screen.links = BuildGroupLinkRows(
        facts.links,
        [&names](const std::string& account) {
          auto found = names.find(account);
          return found != names.end() ? found->second : ShortRelayId(account);
        },
        facts.self_identity, facts.now_ms);
    screen.show_links = true;
  }
  return screen;
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
    case CloseChatPlan::Kind::Dissolve:
      return "dissolve";
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
  showing_archived_ai_ = false;
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

ChatListBucket ClassifyForList(const bool is_ai, const bool listed, const bool archived, const bool showing_archived,
                               const bool showing_archived_ai) {
  if (is_ai) {
    // An AI thread with no readable content yet (not `listed`) shows nowhere until its first message.
    return listed && archived == showing_archived_ai ? ChatListBucket::Ai : ChatListBucket::Hidden;
  }
  return archived == showing_archived ? ChatListBucket::Human : ChatListBucket::Hidden;
}

ArchiveViews ResolveArchiveViews(const std::vector<ArchiveItem>& items, const bool showing_archived,
                                 const bool showing_archived_ai) {
  ArchiveViews views;
  for (const ArchiveItem& item : items) {
    if (!item.archived) {
      continue;
    }
    if (!item.is_ai) {
      ++views.archived_count;
    } else if (item.listed) { // an unlisted AI thread is not shown, so not counted
      ++views.archived_count_ai;
    }
  }
  views.showing_archived = showing_archived && views.archived_count > 0;
  views.showing_archived_ai = showing_archived_ai && views.archived_count_ai > 0;
  return views;
}

void ChatPresenter::Refresh() {
  ChatListScreen next;
  next.showing_archived = showing_archived_;
  next.showing_archived_ai = showing_archived_ai_;
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
  std::vector<ArchiveItem> items;
  items.reserve(sorted.size());
  for (const Thread& thread : sorted) {
    items.push_back({thread.kind == ThreadKind::Ai, thread.listed, prefs_of(thread).archived});
  }
  const ArchiveViews views = ResolveArchiveViews(items, showing_archived_, showing_archived_ai_);
  next.archived_count = views.archived_count;
  next.archived_count_ai = views.archived_count_ai;
  next.showing_archived = showing_archived_ = views.showing_archived; // the last archived one left: back to the list
  next.showing_archived_ai = showing_archived_ai_ = views.showing_archived_ai;
  const std::string active_id = facade_->ActiveThreadId();
  const int64_t now_ms = util::NowUnixMs();
  const std::map<std::string, int> mentions = facade_->MentionCounts();
  std::string local_account;
  if (auto identity = facade_->GetIdentity()) {
    local_account = identity->account_id;
  }
  for (const Thread& thread : sorted) {
    const ThreadLocalPrefs thread_prefs = prefs_of(thread);
    // Each list has its own archive view: an archived AI thread never reaches `rows`, a human one never `ai_rows`.
    const ChatListBucket bucket = ClassifyForList(thread.kind == ThreadKind::Ai, thread.listed, thread_prefs.archived,
                                                  next.showing_archived, next.showing_archived_ai);
    if (bucket == ChatListBucket::Hidden) {
      continue;
    }
    ChatSessionRow row;
    row.id = thread.id;
    const PeerDisplayLabel label = facade_->ResolveThreadLabel(thread);
    row.title = label.title;
    row.from_stranger = IsStrangerChat(thread.kind, label);
    // The stored default AI title is English; the renderer shows it in the UI language.
    row.title_is_default_ai = thread.kind == ThreadKind::Ai && row.title == "New chat";
    const std::string text = GroupEventPreviewOr(thread, local_account);
    // A new thread has no preview yet; an empty second line would collapse the row, so say so in words.
    SessionPreviewShown preview = ResolveSessionPreview(
        SessionPreviewLine(callbacks_.prose ? callbacks_.prose(text) : text), Tr("sidebar.preview_empty"));
    row.preview = std::move(preview.text);
    row.preview_empty = preview.placeholder;
    row.kind = ChatSessionKindName(thread);
    row.unread_count = thread.unread_count;
    row.unread_dot = SessionShowsUnreadDot(thread.unread_count, thread_prefs.marked_unread);
    if (const auto it = mentions.find(thread.id); it != mentions.end() && it->second > 0) {
      row.mentioned = true;
      row.mention_count = it->second;
    }
    row.pinned = thread_prefs.pinned_at != 0;
    row.muted = ThreadIsMuted(thread_prefs.muted_until, now_ms);
    row.date_label = SessionDateLabel(thread.updated_at, now_ms);
    row.updated_at = thread.updated_at;
    row.active = thread.id == active_id;
    {
      const AvatarGlyph glyph = MakeAvatarGlyph(row.title, label.stable_id);
      row.avatar_letter = glyph.letter;
      row.avatar_tone = glyph.tone;
    }
    if (thread.kind == ThreadKind::Direct && label.contact_id) {
      row.contact_id = *label.contact_id;
      row.icon_path = facade_->ContactIconPath(*label.contact_id);
      // Kenneth 2026-11-04: a blocked person's chat must be findable again to unblock them.
      if (auto contact = facade_->GetContact(*label.contact_id); contact && *contact) {
        row.blocked = (*contact)->trust == TrustLevel::Blocked;
      }
    }
    // AI conversations live on the AI 助手 page, not the 聊天 list; an AI thread with no readable content yet
    // (not `listed`) shows nowhere until its first message. Human chats stay in `rows`.
    if (thread.kind == ThreadKind::Ai) {
      // An AI conversation never carries an "unread" badge: its answer is to your own question, read as it arrives,
      // not a message from someone else waiting to be seen (Kenneth 2026-11-05).
      row.unread_count = 0;
      row.unread_dot = false;
      next.ai_rows.push_back(std::move(row));
    } else {
      next.rows.push_back(std::move(row));
    }
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

void ChatPresenter::ToggleArchivedAi() {
  showing_archived_ai_ = !showing_archived_ai_;
  Refresh();
}

void ChatPresenter::ToggleArchived() {
  showing_archived_ = !showing_archived_;
  Refresh();
}

bool ChatPresenter::ApplyDeliveryNotice(const DeliveryNotice& notice) {
  const DeliveryNoticePlan plan = PlanDeliveryNotice(notice, util::NowUnixMs());
  switch (plan.action) {
    case DeliveryNoticePlan::Action::Nothing:
      return false;
    case DeliveryNoticePlan::Action::Toast:
      return true;
    case DeliveryNoticePlan::Action::TranscriptRow:
      break;
  }
  IThreadStore* store = facade_ ? facade_->ThreadStore() : nullptr;
  if (!store || !plan.row) {
    return true;
  }
  // The same failure reported twice (each group member that missed, a retry) is one line.
  if (auto present = store->HasMessageId(plan.row->thread_id, plan.row->id); present && *present) {
    return false;
  }
  return !store->AppendMessage(*plan.row);
}

std::string ChatPresenter::GroupEventPreviewOr(const Thread& thread, const std::string& local_account) {
  // The latest row is a system line stored in English (a joined invitation, a blocked inviter, a rotate notice):
  // the same words as its transcript line, in the UI language. Only a System row: a person may type these words.
  if (LegacySystemLineText(thread.preview)) {
    auto page = facade_->GetMessagesPage(thread.id, std::nullopt, 1);
    if (page && !page->empty() && page->back().content_type == ChatContentType::System) {
      if (auto line = LegacySystemLineText(page->back().text)) {
        return *line;
      }
    }
  }
  // A group whose latest row is a membership line previews the same words as its transcript shows (the stored
  // preview is the English text). Only a preview that is such a text costs a read of the latest message.
  if (thread.kind != ThreadKind::Group || !IsStoredGroupEventText(thread.preview)) {
    return thread.preview;
  }
  auto page = facade_->GetMessagesPage(thread.id, std::nullopt, 1);
  if (!page || page->empty()) {
    return thread.preview;
  }
  ConversationsFacade* facade = facade_;
  const auto line = GroupEventText(
      page->back(), [facade](const std::string& account) { return facade->MemberPublicNameOf(account); },
      local_account);
  return line ? *line : thread.preview;
}

void ChatPresenter::SetPinned(const std::string& thread_id, const bool pinned) {
  if (pinned && facade_) {
    facade_->KeepThreadListed(thread_id);
  }
  UpdatePrefs(thread_id, [pinned](ThreadLocalPrefs& prefs) { prefs.pinned_at = pinned ? util::NowUnixMs() : 0; });
}

void ChatPresenter::Mute(const std::string& thread_id, const SessionMuteChoice choice) {
  UpdatePrefs(thread_id,
              [choice](ThreadLocalPrefs& prefs) { prefs.muted_until = MutedUntilFor(choice, util::NowUnixMs()); });
}

void ChatPresenter::Unmute(const std::string& thread_id) {
  UpdatePrefs(thread_id, [](ThreadLocalPrefs& prefs) {
    prefs.muted_until = 0;
    prefs.mute_mentions = false; // a mute's own setting: the next mute starts with mentions alerting
  });
}

void ChatPresenter::SetMuteMentions(const std::string& thread_id, const bool mute) {
  UpdatePrefs(thread_id, [mute](ThreadLocalPrefs& prefs) { prefs.mute_mentions = mute; });
}

void ChatPresenter::AskBeforeSharingAi(const std::string& thread_id) {
  if (!facade_ || !facade_->ClearSharedAiConfirmed(thread_id)) {
    if (callbacks_.prefs_failed) {
      callbacks_.prefs_failed();
    }
    Changed();
    return;
  }
  ForgetCachedPrefs(); // the facade wrote the stored row, not the cache
  Refresh();
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

Roe<std::string> ChatPresenter::BlockStranger(const std::string& peer_id) {
  auto added = AddPeerToContacts(peer_id);
  if (!added) {
    return added;
  }
  // The add and the block are two writes to the same store: a failed block is retried once, and if it still fails the
  // user is told the person was added but not blocked (there is no contact removal on this facade to roll back with).
  auto set = facade_->SetContactTrust(*added, TrustLevel::Blocked);
  if (!set) {
    set = facade_->SetContactTrust(*added, TrustLevel::Blocked);
  }
  if (!set) {
    return Error("block stranger: added but not blocked").WithUser(Tr("chat.block.added_not_blocked"));
  }
  return added;
}

ChatGroupSheet ChatPresenter::GroupSheetOf(const std::string& group_id) {
  ChatGroupSheet sheet;
  if (!facade_ || facade_->IsGroupDissolved(group_id)) {
    return sheet; // an ended group has no owner to act and no one to remove (G016)
  }
  std::string local_identity;
  if (auto identity = facade_->GetIdentity()) {
    local_identity = identity->account_id;
  }
  // The engine decides by the group's recorded owner; the roster's role column can disagree.
  if (auto owner = facade_->IsLocalOwner(group_id)) {
    sheet.is_owner = *owner;
  }
  sheet.owner_unreachable = facade_->IsOwnerUnreachable(group_id);
  if (auto preset = facade_->GroupPresetOf(group_id)) {
    sheet.preset = *preset;
  }
  const MemberRole role =
      sheet.is_owner ? MemberRole::Owner : facade_->LocalGroupRole(group_id).value_or(MemberRole::Member);
  sheet.may_rename_for_everyone = GroupAllows(GroupAction::Rename, role, sheet.preset);
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

GroupInfoScreen ChatPresenter::GroupInfoOf(const std::string& thread_id) {
  if (!facade_) {
    return {};
  }
  auto thread = facade_->GetThread(thread_id);
  if (!thread || !*thread || (*thread)->kind != ThreadKind::Group || !(*thread)->group_id) {
    return {};
  }
  GroupInfoFacts facts;
  facts.thread_id = thread_id;
  facts.group_id = *(*thread)->group_id;
  facts.title = facade_->ResolveThreadLabel(**thread).title;
  facts.has_local_title = !(*thread)->local_title.empty();
  facts.dissolved = facade_->IsGroupDissolved(facts.group_id);
  if (std::optional<ThreadLocalPrefs> prefs = PrefsOf(thread_id)) {
    facts.muted = ThreadIsMuted(prefs->muted_until, util::NowUnixMs());
  }
  if (facts.dissolved) {
    return BuildGroupInfoScreen(facts);
  }
  if (auto owner = facade_->IsLocalOwner(facts.group_id)) {
    facts.is_owner = *owner; // the engine decides by the group's recorded owner, as everywhere else
  }
  facts.viewer_role = facade_->LocalGroupRole(facts.group_id).value_or(MemberRole::Member);
  if (auto preset = facade_->GroupPresetOf(facts.group_id)) {
    facts.preset = *preset;
  }
  if (auto identity = facade_->GetIdentity()) {
    facts.self_identity = identity->account_id;
  }
  const std::vector<std::string> unreachable_list = facade_->ListUnreachableMembers(facts.group_id);
  const std::unordered_set<std::string> unreachable(unreachable_list.begin(), unreachable_list.end());
  if (auto roster = facade_->ListGroupRoster(facts.group_id)) {
    for (const GroupRosterMember& member : *roster) {
      if (member.member_identity.empty()) {
        continue;
      }
      // This device's row: its account id (the demo app's groups store the local self id instead).
      if (member.member_identity == facts.self_identity || member.member_identity == kLocalSelfContactId) {
        facts.self_identity = member.member_identity;
        GroupMemberRow self = BuildGroupMemberRow(member, std::nullopt, false);
        self.title = Tr("people_picker.you");
        facts.members.push_back(std::move(self));
        continue;
      }
      std::optional<Contact> contact;
      if (!member.contact_id.empty()) {
        if (auto loaded = facade_->GetContact(member.contact_id); loaded && *loaded) {
          contact = **loaded;
        }
      }
      if (!contact) {
        if (auto found = facade_->FindContactByIdentity(member.member_identity, mesh::ContactIdKind::Account);
            found && *found) {
          contact = **found;
        }
      }
      const std::string nickname = contact ? std::string() : facade_->KeptCardNickname(member.member_identity);
      facts.members.push_back(
          BuildGroupMemberRow(member, contact, unreachable.count(member.member_identity) > 0, nickname));
    }
  }
  facts.links = facade_->GroupInviteLinks(facts.group_id);
  facts.now_ms = util::NowUnixMs();
  return BuildGroupInfoScreen(facts);
}

Roe<GroupLinkRow> ChatPresenter::InviteLinkFor(const std::string& group_id) {
  if (!facade_) {
    return Error(Tr("chat.group.err.generic"));
  }
  if (std::optional<GroupLinkRow> ours = facade_->OwnInviteLink(group_id)) {
    return *ours;
  }
  auto made = facade_->MakeInviteLink(group_id);
  if (!made) {
    log().warning << "MakeInviteLink failed: " << made.error().message;
    return Error(GroupActionFailureText(made.error()));
  }
  auto local = facade_->GetIdentity();
  return GroupLinkRow{.link_id = made->link_id,
                      .maker = local ? local->account_id : std::string(),
                      .expires_at = made->expires_at,
                      .max_uses = made->max_uses,
                      .text = made->text,
                      .can_revoke = true};
}

Roe<void> ChatPresenter::RevokeInviteLink(const std::string& group_id, const std::string& link_id) {
  if (!facade_) {
    return Error(Tr("chat.group.err.generic"));
  }
  if (auto revoked = facade_->RevokeInviteLink(group_id, link_id); !revoked) {
    log().warning << "RevokeInviteLink failed: " << revoked.error().message;
    return Error(GroupActionFailureText(revoked.error()));
  }
  return {};
}

std::string GroupActionFailureText(const Error& error) {
  if (error.message == group_roster_sync::kGroupDissolvedError) {
    return Tr(group_roster_sync::kGroupDissolvedError);
  }
  if (error.message == kGroupFullError) {
    return Tr("chat.group.err.full");
  }
  if (error.message == "Group title required") {
    return Tr("chat.group.title_required");
  }
  if (error.message.rfind("Only the group owner", 0) == 0) {
    return Tr("chat.group.err.owner_only");
  }
  for (const char* key :
       {kGroupInviteNotAllowedError, kGroupInviteRemovedError, kGroupAlreadyMemberError, kGroupRenameNotAllowedError,
        kGroupLinkNotAllowedError, kGroupLinkRevokeNotAllowedError, kGroupLinkGoneError}) {
    if (error.message == key) {
      return Tr(key);
    }
  }
  return Tr("chat.group.err.generic");
}

Roe<void> ChatPresenter::SetThreadLocalTitle(const std::string& thread_id, const std::string& local_title) {
  if (!facade_) {
    return Error(Tr("chat.group.err.generic"));
  }
  if (auto saved = facade_->SetThreadLocalTitle(thread_id, local_title); !saved) {
    log().warning << "SetThreadLocalTitle failed: " << saved.error().message;
    return Error(GroupActionFailureText(saved.error()));
  }
  return {};
}

Roe<void> ChatPresenter::RenameGroupShared(const std::string& group_id, const std::string& title) {
  if (!facade_) {
    return Error(Tr("chat.group.err.generic"));
  }
  if (auto renamed = facade_->RenameGroupShared(group_id, title); !renamed) {
    log().warning << "RenameGroupShared failed: " << renamed.error().message;
    return Error(GroupActionFailureText(renamed.error()));
  }
  facade_->NotifyThreadChanged();
  return {};
}

Roe<void> ChatPresenter::SetGroupPreset(const std::string& group_id, const GroupPreset preset) {
  if (!facade_) {
    return Error(Tr("chat.group.err.generic"));
  }
  if (auto set = facade_->SetGroupPreset(group_id, preset); !set) {
    log().warning << "SetGroupPreset failed: " << set.error().message;
    return Error(GroupActionFailureText(set.error()));
  }
  facade_->NotifyThreadChanged();
  return {};
}

Roe<void> ChatPresenter::RemoveGroupMember(const std::string& group_id, const std::string& member_identity) {
  if (!facade_) {
    return Error(Tr("chat.group.err.generic"));
  }
  if (auto removed = facade_->RemoveMemberByIdentity(group_id, member_identity); !removed) {
    log().warning << "RemoveMemberByIdentity failed: " << removed.error().message;
    return Error(GroupActionFailureText(removed.error()));
  }
  facade_->NotifyThreadChanged();
  return {};
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

std::vector<ReactorRow> BuildReactorRows(const std::vector<TranscriptReaction>& reactions,
                                         const std::function<PeerDisplayLabel(const std::string& sender)>& resolve,
                                         const std::function<std::string(const std::string& sender)>& icon_of,
                                         const std::string& unknown_title) {
  std::vector<ReactorRow> rows;
  for (const TranscriptReaction& chip : reactions) {
    std::vector<ReactorRow> group;
    for (const std::string& sender : chip.senders) {
      ReactorRow row;
      row.emoji = chip.emoji;
      row.identity = sender;
      row.is_self = sender == kLocalSelfContactId;
      const PeerDisplayLabel label = resolve(sender);
      row.title = label.trust == PeerLabelTrust::RawId ? unknown_title : label.title;
      row.stable_id = label.stable_id.empty() ? sender : label.stable_id;
      row.icon_path = icon_of(sender);
      group.push_back(std::move(row));
    }
    std::stable_partition(group.begin(), group.end(), [](const ReactorRow& row) { return row.is_self; });
    rows.insert(rows.end(), std::make_move_iterator(group.begin()), std::make_move_iterator(group.end()));
  }
  return rows;
}

std::vector<ReactorRow> ChatPresenter::ReactorsOf(const std::string& message_id) {
  const std::string thread_id = facade_ ? facade_->ActiveThreadId() : std::string();
  if (thread_id.empty() || message_id.empty()) {
    return {};
  }
  auto message = facade_->GetMessage(thread_id, message_id);
  if (!message || !*message) {
    return {};
  }
  // Reactions come after their message: the transcript from it onward carries them all.
  for (const TranscriptRow& row : facade_->BuildTranscript(thread_id, (*message)->display_order)) {
    if (row.message.id != message_id) {
      continue;
    }
    const TranscriptFacts facts = facade_->TranscriptFactsFor();
    return BuildReactorRows(
        row.reactions, [this](const std::string& sender) { return facade_->ResolveSenderLabel(sender); },
        [&facts](const std::string& sender) { return facts.sender_icon ? facts.sender_icon(sender) : std::string(); },
        Tr("chat.reactions.unknown_member"));
  }
  return {};
}

bool ChatPresenter::MyReactionOn(const std::string& message_id, const std::string& emoji) {
  const std::string thread_id = facade_ ? facade_->ActiveThreadId() : std::string();
  const std::string key = NormalizeEmojiKey(emoji);
  if (thread_id.empty() || message_id.empty() || key.empty()) {
    return false;
  }
  auto page = facade_->GetMessagesPage(thread_id, std::nullopt, 500);
  return page && MyReactionActive(*page, message_id, key);
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
      preview += "\xE2\x80\xA6"; // …
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

/** The roster a group chat's mentions are judged against now; none for a chat that is not a group. */
std::optional<MentionRoster> RosterOfGroupThread(ConversationsFacade* facade, const std::string& thread_id) {
  const std::optional<std::string> group_id = GroupIdOf(facade, thread_id);
  if (!group_id) {
    return std::nullopt;
  }
  MentionRoster roster;
  const GroupPreset preset = facade->GroupPresetOf(*group_id).valueOr(GroupPreset::Open);
  if (auto members = facade->ListGroupRoster(*group_id)) {
    for (const GroupRosterMember& member : *members) {
      roster.members.insert(member.member_identity);
      if (GroupAllows(GroupAction::MentionEveryone, member.role, preset)) {
        roster.everyone_senders.insert(member.member_identity);
      }
    }
  }
  // The owner the signed roster names (G012), whatever role a stale row says.
  if (auto owner = facade->OwnerIdentity(*group_id); owner && !owner->empty()) {
    roster.everyone_senders.insert(*owner);
  }
  if (auto identity = facade->GetIdentity()) {
    roster.local_account = identity->account_id;
  }
  return roster;
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
  return RosterOfGroupThread(facade_, thread_id);
}

std::vector<TextSpan> ChatPresenter::SpansOnRoster(const std::string& thread_id, std::vector<TextSpan> spans) const {
  if (spans.empty()) {
    return spans;
  }
  const std::optional<MentionRoster> roster = MentionRosterOf(thread_id);
  return roster ? KeepMentionsOnRoster(std::move(spans), *roster) : std::vector<TextSpan>{};
}

std::vector<MentionCandidate> ChatPresenter::ReadMentionCandidates(ConversationsFacade* facade,
                                                                   const std::string& thread_id) {
  const std::optional<std::string> group_id = GroupIdOf(facade, thread_id);
  if (!group_id) {
    return {};
  }
  auto members = facade->ListGroupRoster(*group_id);
  const std::optional<MentionRoster> roster = RosterOfGroupThread(facade, thread_id);
  if (!members || !roster || roster->local_account.empty()) {
    return {};
  }
  const auto public_name = [facade](const std::string& account_id) { return facade->MemberPublicNameOf(account_id); };
  // `@everyone` is the owner's and the admins' (G017).
  return AllMentionCandidates(*members, roster->local_account, roster->MayMentionEveryone(roster->local_account),
                              public_name, Tr("chat.mention.everyone_word"));
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
  if (facade_->IsGroupDissolved(plan.group_id)) {
    return plan; // G016: the chat of a group that ended is deleted like any chat; nobody is left to tell
  }
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
    // An owner who closes must hand over or dissolve: when the group's record can't be read, trust the roster's role
    // rather than leaving the group without an owner.
    log().warning << "IsLocalOwner failed, using the roster role: " << owner.error().message;
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
    // Solo, or only unreachable peers: no transfer into a ghost roster — the group ends instead (G016).
    plan.kind = plan.successors.empty() ? CloseChatPlan::Kind::Dissolve : CloseChatPlan::Kind::LeaveAsOwner;
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

OwnerLeaveSheet BuildOwnerLeaveSheet(const std::string& thread_id, const CloseChatPlan& plan,
                                     const std::function<std::string(const std::string&)>& name_of) {
  OwnerLeaveSheet sheet;
  if (plan.kind != CloseChatPlan::Kind::LeaveAsOwner && plan.kind != CloseChatPlan::Kind::Dissolve) {
    return sheet;
  }
  sheet.thread_id = thread_id;
  sheet.group_id = plan.group_id;
  for (const std::string& identity : plan.successors) {
    std::string name = name_of ? name_of(identity) : std::string();
    sheet.candidates.push_back({identity, name.empty() ? ShortRelayId(identity) : name});
  }
  std::stable_sort(sheet.candidates.begin(), sheet.candidates.end(),
                   [](const OwnerLeaveCandidate& a, const OwnerLeaveCandidate& b) { return a.name < b.name; });
  return sheet;
}

std::string HandOverConfirmText(const std::string& name) {
  return Tr("group_leave.confirm", {{"name", name}});
}

std::string HandOverGoneText(const std::string& name) {
  return Tr("group_leave.gone", {{"name", name}});
}

bool HandOverStillPossible(const CloseChatPlan& plan, const std::string& successor) {
  return plan.kind == CloseChatPlan::Kind::LeaveAsOwner &&
         std::find(plan.successors.begin(), plan.successors.end(), successor) != plan.successors.end();
}

OwnerLeaveSheet ChatPresenter::OwnerLeaveSheetOf(const std::string& thread_id) {
  if (!facade_) {
    return {};
  }
  CloseChatPlan plan = PlanClose(thread_id);
  // Never this device's own row (the demo app's groups store the local self id rather than the account id).
  std::erase(plan.successors, std::string(kLocalSelfContactId));
  std::unordered_map<std::string, std::string> contact_of;
  if (!plan.group_id.empty()) {
    if (auto roster = facade_->ListGroupRoster(plan.group_id)) {
      for (const GroupRosterMember& member : *roster) {
        contact_of[member.member_identity] = member.contact_id;
      }
    }
  }
  const auto contact_for = [this, &contact_of](const std::string& identity) {
    std::optional<Contact> contact;
    if (auto it = contact_of.find(identity); it != contact_of.end() && !it->second.empty()) {
      if (auto loaded = facade_->GetContact(it->second); loaded && *loaded) {
        contact = **loaded;
      }
    }
    if (!contact) {
      if (auto found = facade_->FindContactByIdentity(identity, mesh::ContactIdKind::Account); found && *found) {
        contact = **found;
      }
    }
    return contact;
  };
  OwnerLeaveSheet sheet = BuildOwnerLeaveSheet(thread_id, plan, [this, &contact_for](const std::string& identity) {
    // The name the member list shows: the contact's, else the nickname on their card.
    const std::optional<Contact> contact = contact_for(identity);
    if (contact) {
      if (std::string title = FormatContactTitle(*contact); !title.empty()) {
        return title;
      }
    }
    return facade_->KeptCardNickname(identity);
  });
  // The same colour seed as the member list shows for the same person.
  for (OwnerLeaveCandidate& candidate : sheet.candidates) {
    const std::optional<Contact> contact = contact_for(candidate.identity);
    candidate.stable_id = contact ? ContactAvatarStableId(*contact) : candidate.identity;
  }
  return sheet;
}

bool ChatPresenter::CanHandOverTo(const std::string& thread_id, const std::string& successor) {
  return facade_ && HandOverStillPossible(PlanClose(thread_id), successor);
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

Roe<void> ChatPresenter::DissolveGroup(const std::string& group_id) {
  if (!facade_) {
    return Error("Messaging not ready");
  }
  if (auto dissolved = facade_->DissolveGroup(group_id); !dissolved) {
    return dissolved.error();
  }
  Refresh();
  return {};
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

void ChatPresenter::ForgetCachedPrefs() {
  prefs_.reset();
}

void ChatPresenter::ForgetThread(const std::string& thread_id) {
  if (prefs_) {
    prefs_->erase(thread_id);
  }
}

} // namespace pbr
