#include "feature/conversations/InboxController.h"
#include "domain/attachments/AttachmentContent.h"
#include "foundation/runtime/AppRuntime.h"

#include "feature/conversations/GroupMembershipWorkflow.h"
#include "domain/attachments/AttachmentFetchWorkflow.h"
#include "foundation/i18n/LocalizationService.h"
#include "lib/messaging/AttachmentCache.h"
#include "lib/messaging/ChatPayloadCodec.h"
#include "lib/people/DirectChatTargetFromContact.h"
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
  return store_.ListThreads();
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
  MarkThreadRead(thread_id);
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

Roe<Thread> InboxController::CreateNewAiThread() {
  Thread thread;
  thread.id = util::GenerateUuid();
  thread.kind = ThreadKind::Ai;
  thread.title = kDefaultAiThreadTitle;
  thread.preview = "";
  thread.updated_at = util::NowUnixMs();

  auto saved = store_.UpsertThread(thread);
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
  AfterMessageDeleted(thread_id, original);
  return {};
}

void InboxController::AfterMessageDeleted(const std::string& thread_id, const ThreadMessage& original) {
  const std::string& message_id = original.id;
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
  if (auto memory = store_.GetThreadMemory(thread_id); memory && *memory && (*memory)->compacted_through_display_order &&
                                                       original.display_order <= *(*memory)->compacted_through_display_order) {
    (void)store_.ClearThreadMemory(thread_id);
  }
  // The list preview shows the newest message: if that was this one, it must not keep showing its text.
  auto page = store_.GetMessagesPage(thread_id, std::nullopt, 100);
  if (!page) {
    return;
  }
  const ThreadMessage* newest = nullptr;
  for (const ThreadMessage& message : *page) {
    if (message.content_type != ChatContentType::Annotation) {
      newest = &message;
    }
  }
  if (newest && newest->id == message_id) {
    if (auto thread = store_.GetThread(thread_id); thread && *thread) {
      Thread updated = **thread;
      updated.preview = Tr("chat.message.deleted");
      (void)store_.UpsertThread(updated); // updated_at untouched: deleting does not move the chat up the list
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
  if (attachment_downloads_ && before) {
    attachment_downloads_->DeleteMessagesMedia(thread_id, *before, {}); // the history is gone: so are its bytes
  }

  Thread updated = **thread;
  updated.preview = "";
  updated.unread_count = 0;
  updated.updated_at = util::NowUnixMs();
  if (!store_.UpsertThread(updated)) {
    return Error("Failed to update thread after clear");
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

  const std::string title =
      (*contact)->display_name.empty() ? (*contact)->server_nickname : (*contact)->display_name;
  auto thread = store_.FindOrCreateDirectThread(target, contact_id, title);
  if (!thread) {
    return thread.error();
  }
  return OpenThread(thread->id);
}

Roe<Thread> InboxController::CreateGroup(const std::string& title,
                                         const std::vector<std::string>& member_contact_ids) {
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
  auto thread = store_.GetThread(thread_id);
  if (!thread || !*thread) {
    return;
  }
  Thread updated = **thread;
  updated.unread_count += delta;
  updated.updated_at = util::NowUnixMs();
  (void)store_.UpsertThread(updated);
}

void InboxController::OnInboundMessagePersisted(const std::string& thread_id,
                                                const std::optional<std::string>& preview) {
  if (thread_id.empty()) {
    return;
  }
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
  if (thread_id != active_thread_id_) {
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
  auto threads = store_.ListThreads();
  if (!threads) {
    return 0;
  }
  int total = 0;
  for (const Thread& thread : *threads) {
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

Roe<void> InboxController::UpdatePreview(const std::string& thread_id, const std::string& preview) {
  auto thread = store_.GetThread(thread_id);
  if (!thread) {
    return thread.error();
  }
  if (!*thread) {
    return Error("Thread not found");
  }
  Thread updated = **thread;
  updated.preview = preview;
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
  auto thread = store_.GetThread(thread_id);
  if (!thread) {
    return thread.error();
  }
  if (!*thread) {
    return Error("Thread not found");
  }
  if ((*thread)->kind != ThreadKind::Group) {
    return Error("Local titles are only supported for groups");
  }
  Thread updated = **thread;
  updated.local_title = local_title;
  updated.updated_at = util::NowUnixMs();
  if (auto saved = store_.UpsertThread(updated); !saved) {
    return saved.error();
  }
  NotifyThreadChanged();
  return {};
}

Roe<void> InboxController::NameAiThreadFromFirstMessage(const std::string& thread_id, const std::string& title) {
  auto thread = store_.GetThread(thread_id);
  if (!thread) {
    return thread.error();
  }
  if (!*thread) {
    return Error("Thread not found");
  }
  if ((*thread)->kind != ThreadKind::Ai || (*thread)->title != kDefaultAiThreadTitle || title.empty()) {
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
  if (shadows_ && (sender_contact_id.rfind("account:", 0) == 0 || sender_contact_id.rfind("relay:", 0) == 0)) {
    shadows_->EnsureLookup(sender_contact_id);
  }
  return labels_.ResolveSender(sender_contact_id).title;
}

bool InboxController::HasLocalMessagesBefore(const std::string& thread_id,
                                             int64_t before_display_order) const {
  auto page = store_.GetMessagesPage(thread_id, before_display_order, 1);
  return page && !page->empty();
}

std::vector<TranscriptRow> InboxController::BuildTranscript(
    const std::string& thread_id, std::optional<int64_t> oldest_inclusive,
    std::optional<int64_t> newest_inclusive) const {
  std::vector<ThreadMessage> messages;
  std::optional<int64_t> before;
  for (;;) {
    auto page = store_.GetMessagesPage(thread_id, before, kDefaultMessagesPageSize);
    if (!page || page->empty()) {
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
                                    [&](const ThreadMessage& m) {
                                      return m.display_order < *oldest_inclusive;
                                    }),
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
                                  [&](const ThreadMessage& m) {
                                    return m.display_order > *newest_inclusive;
                                  }),
                   messages.end());
  }

  return AssembleTranscript(messages, controls_);
}

TranscriptFacts InboxController::Facts() const {
  TranscriptFacts facts;
  facts.sender_label = [this](const std::string& contact_id) { return ResolveSenderLabel(contact_id); };
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
  if (AttachmentAllowsInlinePrivateView(fields->mime, fields->byte_length)) {
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
    // Large private video: keep plaintext off disk until explicit open.
    view.local_path =
        AttachmentLocalPath(profile_data_dir_, message.thread_id, fields->content_hash, fields->mime, fields->filename);
  }
  if (attachment_downloads_) {
    view.state = attachment_downloads_->StateFor(message.thread_id, fields->content_hash, fields->byte_length);
  } else if (!view.local_path.empty() ||
             (!profile_data_dir_.empty() && AttachmentBlobExists(profile_data_dir_, fields->content_hash))) {
    view.state = AttachmentFetchWorkflow::DownloadState::Ready;
  }
  if (IsAttachmentVideoMime(fields->mime) && !profile_data_dir_.empty() &&
      AttachmentPosterExists(profile_data_dir_, message.thread_id, fields->content_hash)) {
    view.poster_path = AttachmentPosterPath(profile_data_dir_, message.thread_id, fields->content_hash);
  }
  return view;
}

} // namespace pbr
