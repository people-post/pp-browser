#include "feature/conversations/InboxController.h"
#include "lib/people/ProfileIconCache.h"
#include "domain/attachments/AttachmentContent.h"
#include "common/runtime/AppRuntime.h"

#include "feature/conversations/GroupMembershipWorkflow.h"
#include "domain/attachments/AttachmentFetchWorkflow.h"
#include "common/Localized.h"
#include "common/ImageMetadata.h"
#include "lib/messaging/AiThreadTitle.h"
#include "lib/messaging/AttachmentCache.h"
#include "lib/messaging/EditMessage.h"
#include "lib/delivery/ChatPayloadCodec.h"
#include "lib/delivery/DirectChatTargetFromContact.h"
#include "lib/messaging/GroupMembershipCodec.h"
#include "lib/voice/AudioFilePlayer.h"
#include "lib/voice/VoiceFile.h"
#include "common/chat/MessagingLimits.h"
#include "common/Utilities.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

#include "common/ValueJson.h"
#include "common/PbrCompat.h"

namespace pbr {

InboxController::InboxController(IThreadStore& store, ContactsStore& contacts, PeerDisplayResolver& labels,
                                 DirectoryShadowCache* shadows)
    : store_(store), contacts_(contacts), labels_(labels), shadows_(shadows) {
  redirectLogger("InboxController");
}

void InboxController::SetGroupMembership(GroupMembershipWorkflow* groups) {
  groups_ = groups;
}

Roe<std::vector<Thread>> InboxController::ListThreads() {
  auto threads = store_.ListThreads();
  if (!threads) {
    return threads;
  }
  // A direct chat made only by a group's membership events (they travel as direct messages) is not a chat the
  // user has: it stays out until something readable is in it, or they open it (the store's `listed` flag, which
  // once set stays — a cleared or renamed chat is never dropped).
  threads->erase(
      std::remove_if(threads->begin(), threads->end(),
                     [](const Thread& thread) { return thread.kind == ThreadKind::Direct && !thread.listed; }),
      threads->end());
  return threads;
}

void InboxController::KeepListed(const std::string& thread_id) {
  std::lock_guard<std::mutex> lock(thread_row_mu_);
  auto thread = store_.GetThread(thread_id);
  if (thread && *thread && !(*thread)->listed) {
    Thread listed = **thread;
    listed.listed = true;
    (void)store_.UpsertThread(listed);
  }
}

Roe<Thread> InboxController::GetActiveThread() const {
  if (active_thread_id_.empty()) {
    return Error("No active thread");
  }
  auto thread = store_.GetThread(active_thread_id_);
  if (!thread) {
    return thread.error();
  }
  if (!*thread) {
    return Error("Active thread not found");
  }
  return **thread;
}

Roe<Thread> InboxController::OpenThread(const std::string& thread_id) {
  auto thread = store_.GetThread(thread_id);
  if (!thread) {
    return thread.error();
  }
  if (!*thread) {
    return Error("Thread not found");
  }

  active_thread_id_ = thread_id;
  KeepListed(thread_id); // a chat the user opened is theirs, even before anything is said in it
  MarkThreadRead(thread_id);
  if (mentions_) {
    (void)mentions_->MarkThreadRead(thread_id); // read: "You were mentioned" goes with the unread count
  }
  if (on_thread_changed_) {
    on_thread_changed_();
  }
  return **thread;
}

void InboxController::ClearActiveThread() {
  if (active_thread_id_.empty()) {
    return;
  }
  active_thread_id_.clear();
  if (on_thread_changed_) {
    on_thread_changed_();
  }
}

Roe<Thread> InboxController::CreateAiThread() {
  Thread thread;
  thread.id = util::GenerateUuid();
  thread.kind = ThreadKind::Ai;
  thread.title = kDefaultAiThreadTitle;
  thread.preview = "";
  thread.updated_at = util::NowUnixMs();
  return store_.UpsertThread(thread);
}

Roe<Thread> InboxController::CreateNewAiThread() {
  auto saved = CreateAiThread();
  if (!saved) {
    return saved.error();
  }
  return OpenThread(saved->id);
}

Roe<void> InboxController::CloseThread(const std::string& thread_id) {
  auto thread = store_.GetThread(thread_id);
  if (!thread) {
    return thread.error();
  }
  if (!*thread) {
    return Error("Thread not found");
  }

  if ((*thread)->kind == ThreadKind::Group && (*thread)->group_id && groups_) {
    // Always clear local membership before deleting the thread so inbound traffic cannot resurrect it.
    (void)groups_->DismissLocalGroup(*(*thread)->group_id);
  }

  const bool was_active = active_thread_id_ == thread_id;
  // Read what the thread refers to before its rows go; delete the bytes only once the thread is gone.
  auto messages = store_.GetMessagesPage(thread_id, std::nullopt, 10000);
  auto deleted = store_.DeleteThread(thread_id);
  if (!deleted || !*deleted) {
    return Error("Failed to delete thread");
  }
  if (mentions_) {
    (void)mentions_->DropThread(thread_id);
  }
  if (attachment_downloads_) {
    attachment_downloads_->ForgetThread(thread_id);
    if (messages) {
      attachment_downloads_->DeleteMessagesMedia(thread_id, *messages, {});
    }
  }

  if (was_active) {
    active_thread_id_.clear();
    auto threads = store_.ListThreads();
    if (threads) {
      for (const Thread& candidate : *threads) {
        if (candidate.id == thread_id) {
          continue;
        }
        if (auto opened = OpenThread(candidate.id)) {
          return {};
        }
      }
    }
    if (on_thread_changed_) {
      on_thread_changed_();
    }
  } else if (on_thread_changed_) {
    on_thread_changed_();
  }
  return {};
}

Roe<void> InboxController::DeleteMessageForMe(const std::string& thread_id, const std::string& message_id) {
  auto found = store_.GetMessage(thread_id, message_id);
  if (!found) {
    return found.error();
  }
  if (!found->has_value()) {
    return Error("Message not found");
  }
  if ((*found)->content_type == ChatContentType::Deleted) {
    return {};
  }
  const ThreadMessage original = **found;
  // The row first (the part that can fail); its bytes only once the content is gone for good.
  auto deleted = store_.DeleteMessageContent(thread_id, message_id);
  if (!deleted) {
    return deleted.error();
  }
  if (!*deleted) {
    return Error("Message not found");
  }
  // Its edit rows go blank too, and an edit of ours still queued is not sent: no edited words outlive the message
  // (DEV/PLAN-message-edit.md).
  (void)BlankEditRowsFor(store_, thread_id, message_id, {}, cancel_pending_send_);
  AfterMessageDeleted(thread_id, original);
  return {};
}

void InboxController::AfterMessageEdited(const std::string& thread_id, const ThreadMessage& before,
                                         const ThreadMessage& after) {
  if (mentions_) {
    (void)mentions_->DropMessage(thread_id, before.id); // the edited text is plain: no "you were mentioned" left
  }
  if (auto memory = store_.GetThreadMemory(thread_id);
      memory && *memory && (*memory)->compacted_through_display_order &&
      before.display_order <= *(*memory)->compacted_through_display_order) {
    (void)store_.ClearThreadMemory(thread_id); // its summary still carries the old words
  }
  auto page = store_.GetMessagesPage(thread_id, std::nullopt, 100);
  if (!page) {
    return;
  }
  const auto holder = store_.GetThread(thread_id);
  const bool direct_thread = holder && *holder && (*holder)->kind == ThreadKind::Direct;
  const ThreadMessage* newest = nullptr;
  for (const ThreadMessage& message : *page) {
    if (message.content_type != ChatContentType::Annotation &&
        !(direct_thread && GroupMembershipCodec::IsHiddenInDirectThread(message))) {
      newest = &message;
    }
  }
  if (!newest || newest->id != before.id) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(thread_row_mu_);
    auto thread = store_.GetThread(thread_id);
    if (!thread || !*thread) {
      return;
    }
    Thread updated = **thread;
    updated.preview = after.text;
    (void)store_.UpsertThread(updated); // updated_at untouched: an edit does not move the chat up the list
  }
  if (on_thread_changed_) {
    if (AppRuntime::IsRunning()) {
      AppRuntime::PostUI([this]() {
        if (on_thread_changed_) {
          on_thread_changed_();
        }
      });
    } else {
      on_thread_changed_();
    }
  }
}

void InboxController::AfterMessageDeleted(const std::string& thread_id, const ThreadMessage& original) {
  const std::string& message_id = original.id;
  if (mentions_) {
    (void)mentions_->DropMessage(thread_id, message_id); // a deleted mention leaves no marker (G015)
  }
  if (attachment_downloads_ && (original.content_type == ChatContentType::Attachment || original.ai_image_json)) {
    // The rest of the thread, so bytes another message here still shows are kept. Reading it decrypts the
    // transcript: off the UI thread.
    auto delete_media = [this, thread_id, original]() {
      auto rest = store_.GetMessagesPage(thread_id, std::nullopt, 10000);
      attachment_downloads_->DeleteMessagesMedia(thread_id, {original}, rest ? *rest : std::vector<ThreadMessage>{});
    };
    if (AppRuntime::IsRunning()) {
      AppRuntime::PostWorkerNormal(std::move(delete_media));
    } else {
      delete_media();
    }
  }
  // A summary that already folded this message in still carries its words: forget it (it is rebuilt from
  // what is left).
  if (auto memory = store_.GetThreadMemory(thread_id);
      memory && *memory && (*memory)->compacted_through_display_order &&
      original.display_order <= *(*memory)->compacted_through_display_order) {
    (void)store_.ClearThreadMemory(thread_id);
  }
  // The list preview shows the newest message: if that was this one, it must not keep showing its text.
  auto page = store_.GetMessagesPage(thread_id, std::nullopt, 100);
  if (!page) {
    return;
  }
  const auto holder = store_.GetThread(thread_id);
  const bool direct_thread = holder && *holder && (*holder)->kind == ThreadKind::Direct;
  const ThreadMessage* newest = nullptr;
  for (const ThreadMessage& message : *page) {
    if (message.content_type != ChatContentType::Annotation &&
        !(direct_thread && GroupMembershipCodec::IsHiddenInDirectThread(message))) {
      newest = &message;
    }
  }
  if (newest && newest->id == message_id) {
    bool row_updated = false;
    {
      std::lock_guard<std::mutex> lock(thread_row_mu_);
      if (auto thread = store_.GetThread(thread_id); thread && *thread) {
        Thread updated = **thread;
        updated.preview = Localized("chat.message.deleted");
        (void)store_.UpsertThread(updated); // updated_at untouched: deleting does not move the chat up the list
        row_updated = true;
      }
    }
    if (row_updated) {
      // Reached from workers too (delete for everyone, a peer's retract): the UI refresh runs on the UI thread.
      if (on_thread_changed_) {
        if (AppRuntime::IsRunning()) {
          AppRuntime::PostUI([this]() {
            if (on_thread_changed_) {
              on_thread_changed_();
            }
          });
        } else {
          on_thread_changed_();
        }
      }
    }
  }
}

Roe<void> InboxController::ClearThreadHistory(const std::string& thread_id, const bool forget_memory) {
  auto thread = store_.GetThread(thread_id);
  if (!thread) {
    return thread.error();
  }
  if (!*thread) {
    return Error("Thread not found");
  }

  auto before = store_.GetMessagesPage(thread_id, std::nullopt, 10000); // what the bytes belong to
  if (attachment_downloads_) {
    if (auto prepared = attachment_downloads_->PrepareThreadHistoryClear(thread_id, store_); !prepared) {
      return prepared.error();
    }
  }

  if (auto cleared = store_.ClearMessages(thread_id, ClearMessagesOptions{.forget_memory = forget_memory}); !cleared) {
    return cleared.error();
  }
  if (mentions_) {
    (void)mentions_->DropThread(thread_id); // the messages that mentioned us are gone: so is "You were mentioned"
  }
  if (attachment_downloads_ && before) {
    attachment_downloads_->DeleteMessagesMedia(thread_id, *before, {}); // the history is gone: so are its bytes
  }

  {
    std::lock_guard<std::mutex> lock(thread_row_mu_);
    auto fresh =
        store_.GetThread(thread_id); // the row as it is now: an ingest may have changed it since the read above
    Thread updated = fresh && *fresh ? **fresh : **thread;
    updated.preview = "";
    updated.unread_count = 0;
    updated.updated_at = util::NowUnixMs();
    if (!store_.UpsertThread(updated)) {
      return Error("Failed to update thread after clear");
    }
  }

  if (on_thread_changed_) {
    on_thread_changed_();
  }
  return {};
}

Roe<void> InboxController::ForgetThreadMemory(const std::string& thread_id) {
  auto thread = store_.GetThread(thread_id);
  if (!thread) {
    return thread.error();
  }
  if (!*thread) {
    return Error("Thread not found");
  }
  if ((*thread)->kind != ThreadKind::Ai) {
    return Error("Forget memory is only available for AI threads");
  }

  if (auto cleared = store_.ClearThreadMemory(thread_id); !cleared) {
    return cleared.error();
  }

  if (on_thread_changed_) {
    on_thread_changed_();
  }
  return {};
}

Roe<Thread> InboxController::FindOrCreateDirectThread(const std::string& contact_id) {
  return FindOrCreateDirectThread(contact_id, ThreadChannel::E2ePublic);
}

Roe<Thread> InboxController::FindOrCreateDirectThread(const std::string& contact_id, ThreadChannel channel) {
  auto thread = FindOrCreateDirectThreadQuietly(contact_id, channel);
  if (!thread) {
    return thread.error();
  }
  return OpenThread(thread->id);
}

Roe<Thread> InboxController::FindOrCreateDirectThreadQuietly(const std::string& contact_id, ThreadChannel channel) {
  auto contact = contacts_.Get(contact_id);
  if (!contact) {
    return contact.error();
  }
  if (!*contact) {
    return Error("Contact not found");
  }

  const DirectChatTarget target = DirectChatTargetFromContact(**contact, channel);
  if (target.peer_identity_value.empty()) {
    return Error("Contact has no messaging identity");
  }

  const std::string title = (*contact)->display_name.empty() ? (*contact)->server_nickname : (*contact)->display_name;
  return store_.FindOrCreateDirectThread(target, contact_id, title);
}

Roe<Thread> InboxController::CreateGroup(const std::string& title, const std::vector<std::string>& member_contact_ids) {
  if (!groups_) {
    return Error("Messaging not initialized");
  }
  auto created = groups_->CreateGroup(title, member_contact_ids);
  if (!created) {
    return created.error();
  }
  return OpenThread(created->id);
}

Roe<Thread> InboxController::CreateDirectThread(const std::string& contact_id, ThreadChannel channel) {
  auto contact = contacts_.Get(contact_id);
  if (!contact) {
    return contact.error();
  }
  if (!*contact) {
    return Error("Contact not found");
  }

  const DirectChatTarget target = DirectChatTargetFromContact(**contact, channel);
  if (target.peer_identity_value.empty()) {
    return Error("Contact has no messaging identity");
  }

  Thread thread;
  thread.id = util::GenerateUuid();
  thread.kind = ThreadKind::Direct;
  thread.channel = channel;
  thread.peer_identity_kind = target.peer_identity_kind;
  thread.peer_identity_value = target.peer_identity_value;
  thread.title = (*contact)->display_name.empty() ? (*contact)->server_nickname : (*contact)->display_name;
  thread.participant_contact_ids = {contact_id};
  thread.preview = "";
  thread.updated_at = util::NowUnixMs();
  thread.encrypted = ThreadChannelIsE2e(channel);

  auto saved = store_.FindOrCreateDirectThread(target, contact_id, thread.title);
  if (!saved) {
    return saved.error();
  }
  return OpenThread(saved->id);
}

void InboxController::MarkThreadRead(const std::string& thread_id) {
  std::lock_guard<std::mutex> lock(thread_row_mu_);
  auto thread = store_.GetThread(thread_id);
  if (!thread || !*thread) {
    return;
  }
  if ((*thread)->unread_count == 0) {
    return;
  }
  Thread updated = **thread;
  updated.unread_count = 0;
  (void)store_.UpsertThread(updated);
}

void InboxController::IncrementUnread(const std::string& thread_id, const int delta, const bool even_if_active) {
  if (delta <= 0 || thread_id.empty() || (thread_id == active_thread_id_ && !even_if_active)) {
    return;
  }
  std::lock_guard<std::mutex> lock(thread_row_mu_);
  auto thread = store_.GetThread(thread_id);
  if (!thread || !*thread) {
    return;
  }
  Thread updated = **thread;
  updated.unread_count += delta;
  updated.updated_at = util::NowUnixMs();
  (void)store_.UpsertThread(updated);
}

void InboxController::OnInboundMessagePersisted(const std::string& thread_id, const std::optional<std::string>& preview,
                                                bool attentive) {
  if (thread_id.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(thread_row_mu_);
  auto thread = store_.GetThread(thread_id);
  if (!thread || !*thread) {
    return;
  }

  Thread updated = **thread;
  bool changed = false;
  if (preview && *preview != updated.preview) {
    updated.preview = *preview;
    changed = true;
  }
  if (CountsAsUnread(thread_id, active_thread_id_, attentive)) {
    updated.unread_count += 1;
    changed = true;
  }
  if (!changed) {
    return;
  }
  updated.updated_at = util::NowUnixMs();
  (void)store_.UpsertThread(updated);
}

int InboxController::SumUnread() const {
  return SumUnreadIfKnown().value_or(0);
}

std::optional<int> InboxController::SumUnreadIfKnown() const {
  auto threads = store_.ListThreads();
  if (!threads) {
    return std::nullopt;
  }
  int total = 0;
  for (const Thread& thread : *threads) {
    // AI conversations never count as unread (the assistant's answer to your own question is not a waiting message).
    if (thread.kind == ThreadKind::Ai) {
      continue;
    }
    total += thread.unread_count;
  }
  return total;
}

int InboxController::SumUnreadForContact(const std::string& contact_id) const {
  if (contact_id.empty()) {
    return 0;
  }
  auto threads = store_.ListThreads();
  if (!threads) {
    return 0;
  }
  int total = 0;
  for (const Thread& thread : *threads) {
    if (std::find(thread.participant_contact_ids.begin(), thread.participant_contact_ids.end(), contact_id) ==
        thread.participant_contact_ids.end()) {
      continue;
    }
    total += thread.unread_count;
  }
  return total;
}

Roe<void> InboxController::UpdatePreview(const std::string& thread_id, const std::string& preview,
                                         const bool assistant_text) {
  std::lock_guard<std::mutex> lock(thread_row_mu_);
  auto thread = store_.GetThread(thread_id);
  if (!thread) {
    return thread.error();
  }
  if (!*thread) {
    return Error("Thread not found");
  }
  Thread updated = **thread;
  // The user's own text in an AI thread never shows a code or pasted blob in the row; an answer is shown as is
  // (a long URL or hash in it is not a secret).
  updated.preview = updated.kind == ThreadKind::Ai && !assistant_text && LooksSensitiveForAiTitle(preview)
                        ? Localized("chat.new_chat")
                        : preview;
  updated.updated_at = util::NowUnixMs();
  if (store_.UpsertThread(updated)) {
    return {};
  }
  return Error("Failed to update thread preview");
}

void InboxController::SetProfileDataDir(std::string profile_dir) {
  profile_data_dir_ = std::move(profile_dir);
}

void InboxController::SetAttachmentDownloads(AttachmentFetchWorkflow* downloads) {
  attachment_downloads_ = downloads;
}

void InboxController::SetOnThreadChanged(ThreadChangedCallback callback) {
  on_thread_changed_ = std::move(callback);
}

void InboxController::NotifyThreadChanged() {
  if (on_thread_changed_) {
    on_thread_changed_();
  }
}

PeerDisplayLabel InboxController::ResolveThreadLabel(const Thread& thread) const {
  if (shadows_ && thread.kind == ThreadKind::Direct && !thread.peer_identity_value.empty()) {
    shadows_->EnsureLookup(thread.peer_identity_value);
  }
  return labels_.ResolveThread(thread);
}

Roe<void> InboxController::SetThreadLocalTitle(const std::string& thread_id, const std::string& local_title) {
  {
    std::lock_guard<std::mutex> lock(thread_row_mu_);
    auto thread = store_.GetThread(thread_id);
    if (!thread) {
      return thread.error();
    }
    if (!*thread) {
      return Error("Thread not found");
    }
    if ((*thread)->kind != ThreadKind::Group && (*thread)->kind != ThreadKind::Ai) {
      return Error("Local titles are only supported for groups and AI conversations");
    }
    Thread updated = **thread;
    updated.local_title = local_title;
    updated.updated_at = util::NowUnixMs();
    if (auto saved = store_.UpsertThread(updated); !saved) {
      return saved.error();
    }
  }
  NotifyThreadChanged(); // outside the lock
  return {};
}

Roe<void> InboxController::NameAiThreadFromFirstMessage(const std::string& thread_id,
                                                        const std::string& first_message) {
  std::lock_guard<std::mutex> lock(thread_row_mu_);
  auto thread = store_.GetThread(thread_id);
  if (!thread) {
    return thread.error();
  }
  if (!*thread) {
    return Error("Thread not found");
  }
  if ((*thread)->kind != ThreadKind::Ai || (*thread)->title != kDefaultAiThreadTitle) {
    return {};
  }
  // A sensitive first message (device-link code, PIN, pasted blob) leaves the neutral default title: the title
  // is shown in lists and notifications. A renamed thread's own name lives in local_title and is not touched.
  const std::string title =
      LooksSensitiveForAiTitle(first_message) ? std::string() : AiThreadTitleFromMessage(first_message);
  if (title.empty()) {
    return {};
  }
  Thread updated = **thread;
  updated.title = title;
  if (auto saved = store_.UpsertThread(updated); !saved) {
    return saved.error();
  }
  return {};
}

std::string InboxController::ResolveSenderLabel(const std::string& sender_contact_id) const {
  if (shadows_ && sender_contact_id.rfind("account:", 0) == 0) {
    shadows_->EnsureLookup(sender_contact_id);
  }
  return labels_.ResolveSender(sender_contact_id).title;
}

bool InboxController::HasLocalMessagesBefore(const std::string& thread_id, int64_t before_display_order) const {
  auto page = store_.GetMessagesPage(thread_id, before_display_order, 1);
  return page && !page->empty();
}

std::vector<TranscriptRow> InboxController::BuildTranscript(const std::string& thread_id,
                                                            std::optional<int64_t> oldest_inclusive,
                                                            std::optional<int64_t> newest_inclusive) const {
  std::vector<ThreadMessage> messages;
  std::optional<int64_t> before;
  for (;;) {
    auto page = store_.GetMessagesPage(thread_id, before, kDefaultMessagesPageSize);
    if (!page) {
      // One unreadable row fails the whole page: say so, or the chat just reads empty.
      log().warning << "Transcript not read thread_id=" << thread_id << ": " << page.error().message;
      break;
    }
    if (page->empty()) {
      break;
    }
    const int64_t page_oldest = page->front().display_order;
    const size_t page_size = page->size();
    if (messages.empty()) {
      messages = std::move(*page);
    } else {
      messages.insert(messages.begin(), page->begin(), page->end());
    }
    if (!oldest_inclusive.has_value()) {
      break;
    }
    if (page_oldest <= *oldest_inclusive) {
      messages.erase(std::remove_if(messages.begin(), messages.end(),
                                    [&](const ThreadMessage& m) { return m.display_order < *oldest_inclusive; }),
                     messages.end());
      break;
    }
    if (page_size < kDefaultMessagesPageSize) {
      break;
    }
    before = page_oldest;
  }

  if (newest_inclusive.has_value()) {
    messages.erase(std::remove_if(messages.begin(), messages.end(),
                                  [&](const ThreadMessage& m) { return m.display_order > *newest_inclusive; }),
                   messages.end());
  }

  ThreadKind kind = ThreadKind::Direct;
  if (auto thread = store_.GetThread(thread_id); thread && *thread) {
    kind = (*thread)->kind;
  }
  const bool direct_thread = kind == ThreadKind::Direct;
  std::vector<TranscriptRow> rows = AssembleTranscript(messages, controls_, kind);
  // A run of a group's events on top of a direct chat's history would leave the page blank: go back for older
  // pages until something shows or the history ends (bounded). A caller that asked for a window gets exactly it.
  constexpr int kMaxExtraPages = 20;
  for (int extra = 0;
       direct_thread && rows.empty() && !oldest_inclusive.has_value() && !messages.empty() && extra < kMaxExtraPages;
       ++extra) {
    auto older = store_.GetMessagesPage(thread_id, messages.front().display_order, kDefaultMessagesPageSize);
    if (!older || older->empty()) {
      break;
    }
    messages.insert(messages.begin(), older->begin(), older->end());
    rows = AssembleTranscript(messages, controls_, kind);
  }
  return rows;
}

TranscriptFacts InboxController::Facts() const {
  TranscriptFacts facts;
  facts.sender_label = [this](const std::string& contact_id) { return ResolveSenderLabel(contact_id); };
  facts.sender_stable_id = [this](const std::string& contact_id) {
    return labels_.ResolveSender(contact_id).stable_id;
  };
  facts.sender_icon = [this](const std::string& contact_id) -> std::string {
    // A contact id, or the account of a group member who is not a contact.
    if (auto contact = contacts_.Get(contact_id); contact && *contact) {
      const std::string key = ProfileIconCacheKeyForContact(**contact);
      return key.empty() ? std::string() : ProfileIconLocalPath(profile_data_dir_, key);
    }
    return ProfileIconLocalPathForIdentity(profile_data_dir_, contacts_, contact_id);
  };
  facts.control_line = [this](const ThreadMessage& message) { return ControlLine(message); };
  facts.attachment = [this](const ThreadMessage& message) { return AttachmentView(message); };
  return facts;
}

std::optional<std::string> InboxController::ControlLine(const ThreadMessage& message) const {
  const auto family = controls_ ? controls_->Find(message) : std::nullopt;
  if (!family || !family->line) {
    return std::nullopt;
  }
  SystemControlLabels labels;
  labels.peer = [this](const std::string& identity) { return ResolveSenderLabel(identity); };
  return family->line(message, labels);
}

TranscriptAttachmentView InboxController::AttachmentView(const ThreadMessage& message) const {
  TranscriptAttachmentView view;
  auto fields = ChatPayloadCodec::DecodeAttachmentJson(message.payload_json);
  if (!fields) {
    return view;
  }
  const uint64_t max_receive_bytes = attachment_downloads_ ? attachment_downloads_->Limits().max_receive_bytes
                                                           : DefaultAttachmentLimits().max_receive_bytes;
  // Rendering never decrypts a large file: its plaintext view is made by an explicit Open (on a worker).
  view.on_demand = fields->byte_length > kMaxDirectAttachmentBytes;
  if (!view.on_demand && AttachmentAllowsInlinePrivateView(fields->mime, fields->byte_length, max_receive_bytes)) {
    if (attachment_downloads_) {
      if (auto local = attachment_downloads_->EnsureLocalViewPath(message.thread_id, fields->content_hash, fields->mime,
                                                                  fields->filename)) {
        view.local_path = *local;
      }
    } else if (!profile_data_dir_.empty()) {
      view.local_path = AttachmentLocalPath(profile_data_dir_, message.thread_id, fields->content_hash, fields->mime,
                                            fields->filename);
    }
  } else if (!profile_data_dir_.empty()) {
    // Large file: plaintext stays off disk until an explicit Open; a view made by an earlier Open is used.
    view.local_path =
        AttachmentLocalPath(profile_data_dir_, message.thread_id, fields->content_hash, fields->mime, fields->filename);
  }
  // The pixel-cap verdict and upright size of a picture file, read from its head once per file.
  const auto image_facts = [this](const std::string& path) -> const InlineImageFacts& {
    auto cached = inline_image_ok_.find(path);
    if (cached == inline_image_ok_.end()) {
      const uint64_t max_pixels = attachment_downloads_ ? attachment_downloads_->Limits().max_image_pixels
                                                        : DefaultAttachmentLimits().max_image_pixels;
      InlineImageFacts facts;
      if (const auto header = ReadImageHeader(path)) {
        facts.ok = static_cast<uint64_t>(header->width) * static_cast<uint64_t>(header->height) <= max_pixels;
        // Orientations 5..8 turn the picture a quarter: it shows with its sides swapped.
        const bool turned = header->exif_orientation >= 5 && header->exif_orientation <= 8;
        facts.width = turned ? header->height : header->width;
        facts.height = turned ? header->width : header->height;
      }
      cached = inline_image_ok_.emplace(path, facts).first;
    }
    return cached->second;
  };
  bool wants_preview = false;
  if (IsAttachmentImageMime(fields->mime)) {
    // B203: a bubble shows the picture's preview (kImagePreviewMaxSide), the picture itself only when it is no longer
    // than that, else a placeholder of its size until the preview is made; a picture whose preview cannot be made is
    // a file card. A tap opens the full picture either way.
    const std::string preview = AttachmentPosterPath(profile_data_dir_, message.thread_id, fields->content_hash);
    const bool preview_exists = AttachmentPosterExists(profile_data_dir_, message.thread_id, fields->content_hash);
    const bool preview_failed =
        attachment_downloads_ && attachment_downloads_->ChatPreviewFailed(message.thread_id, fields->content_hash);
    if (!view.on_demand) {
      if (!view.local_path.empty()) {
        // The bytes are a contact's: look at the declared size before the picture is decoded into an <img>.
        const InlineImageFacts& facts = image_facts(view.local_path);
        view.inline_image_ok = facts.ok;
        view.image_width = facts.width;
        view.image_height = facts.height;
        if (facts.ok && ImageNeedsPreview(facts.width, facts.height)) {
          if (preview_exists) {
            view.local_path = preview; // laid out with the picture's own size: the placeholder had it too
          } else if (preview_failed) {
            view.inline_image_ok = false; // a file card that opens the picture (`local_path` stays its view)
          } else {
            view.picture_placeholder = true;
            view.local_path.clear();
            wants_preview = true;
          }
        }
      }
    } else if (preview_exists) {
      // A large picture: its size is the preview's (the large file is not decrypted to draw a bubble).
      view.local_path = preview;
      const InlineImageFacts& facts = image_facts(preview);
      view.inline_image_ok = facts.ok;
      view.image_width = facts.width;
      view.image_height = facts.height;
    } else if (preview_failed) {
      view.inline_image_ok = false; // a file card with Open
    } else {
      // A view an earlier Open made is the full picture: never in the bubble. The placeholder until the preview.
      view.local_path.clear();
      view.picture_placeholder = true;
      view.image_width = kPicturePlaceholderFallbackWidth;
      view.image_height = kPicturePlaceholderFallbackHeight;
      wants_preview = true;
    }
  }
  if (attachment_downloads_) {
    view.state = attachment_downloads_->StateFor(message.thread_id, *fields);
    view.download_percent = attachment_downloads_->ProgressPercent(message.thread_id, fields->content_hash);
    if (wants_preview && view.state == AttachmentFetchWorkflow::DownloadState::Ready) {
      // Made on a worker, never here; only these rows are drawn again when it is done (the chat-preview callback).
      attachment_downloads_->RequestChatPreview(message.thread_id, *fields);
    }
  } else if (!view.local_path.empty() || view.picture_placeholder) {
    view.state = AttachmentFetchWorkflow::DownloadState::Ready;
  }
  if (view.picture_placeholder && view.state != AttachmentFetchWorkflow::DownloadState::Ready) {
    view.picture_placeholder = false; // not downloaded: the download card, as before
  }
  if (IsAttachmentVideoMime(fields->mime) && !profile_data_dir_.empty() &&
      AttachmentPosterExists(profile_data_dir_, message.thread_id, fields->content_hash)) {
    view.poster_path = AttachmentPosterPath(profile_data_dir_, message.thread_id, fields->content_hash);
    // The poster's own size (the frame is laid out for it); read once per poster file.
    auto cached = inline_image_ok_.find(view.poster_path);
    if (cached == inline_image_ok_.end()) {
      InlineImageFacts facts;
      if (const auto header = ReadImageHeader(view.poster_path)) {
        facts.ok = true;
        facts.width = header->width;
        facts.height = header->height;
      }
      cached = inline_image_ok_.emplace(view.poster_path, facts).first;
    }
    view.image_width = cached->second.width;
    view.image_height = cached->second.height;
  }
  if (attachment_downloads_ && !view.local_path.empty() && IsAudioFileAttachment(fields->mime, fields->filename) &&
      !ParseVoiceFileName(fields->filename)) {
    // A sound file's card: its title, artist and cover from its own tags, read on a worker once its plaintext view
    // exists (a large one after its first play); the change notification draws them.
    if (const auto tags = attachment_downloads_->AudioTags(message.thread_id, fields->content_hash)) {
      view.audio_title = tags->title;
      view.audio_artist = tags->artist;
      view.audio_duration_ms = tags->duration_ms;
      if (AttachmentPosterExists(profile_data_dir_, message.thread_id, fields->content_hash)) {
        view.poster_path = AttachmentPosterPath(profile_data_dir_, message.thread_id, fields->content_hash);
      }
    } else {
      attachment_downloads_->RequestAudioTags(message.thread_id, fields->content_hash, view.local_path);
    }
  }
  // Locked, or locking: the DEK goes before the view cache is wiped, so this is read after every file check above. No
  // <img> may name a file of the view cache now (the UI library remembers a failed load for the whole run): a picture
  // is its placeholder, a video or sound file has no cover.
  if (attachment_downloads_ && !attachment_downloads_->HasDek()) {
    view.poster_path.clear();
    if (IsAttachmentImageMime(fields->mime) && view.inline_image_ok &&
        view.state == AttachmentFetchWorkflow::DownloadState::Ready) {
      view.local_path.clear();
      view.picture_placeholder = true;
      if (view.image_width <= 0 || view.image_height <= 0) {
        view.image_width = kPicturePlaceholderFallbackWidth; // its own view could not be read: the fallback box
        view.image_height = kPicturePlaceholderFallbackHeight;
      }
    }
  }
  return view;
}

} // namespace pbr
