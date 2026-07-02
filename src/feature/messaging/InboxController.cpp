#include "feature/messaging/InboxController.h"

#include "base/messaging/DirectChatTarget.h"
#include "base/messaging/MessagingLimits.h"

#include "base/ai/StructuredTextParser.h"
#include "feature/chat/ChatFormHelper.h"
#include "common/Utilities.h"
#include "base/messaging/MessagingJson.h"

namespace pbr {

InboxController::InboxController(IThreadStore& store, ContactsStore& contacts)
    : store_(store), contacts_(contacts) {
  redirectLogger("InboxController");
}

Roe<void> InboxController::EnsureAiHomeThread() {
  if (!ai_home_thread_id_.empty()) {
    if (auto thread = store_.GetThread(ai_home_thread_id_)) {
      if (*thread) {
        return {};
      }
    }
  }

  auto threads = store_.ListThreads();
  if (!threads) {
    return threads.error();
  }

  for (const Thread& thread : *threads) {
    if (thread.kind == ThreadKind::Ai) {
      ai_home_thread_id_ = thread.id;
      if (active_thread_id_.empty()) {
        active_thread_id_ = thread.id;
      }
      return {};
    }
  }

  Thread thread;
  thread.id = util::GenerateUuid();
  thread.kind = ThreadKind::Ai;
  thread.title = "pp-browser";
  thread.preview = "Ask anything...";
  thread.updated_at = util::NowUnixMs();
  auto saved = store_.UpsertThread(thread);
  if (!saved) {
    return saved.error();
  }

  ai_home_thread_id_ = saved->id;
  if (active_thread_id_.empty()) {
    active_thread_id_ = saved->id;
  }
  return {};
}

Roe<std::vector<Thread>> InboxController::ListThreads() {
  auto ensure = EnsureAiHomeThread();
  if (!ensure) {
    return ensure.error();
  }
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

Roe<Thread> InboxController::CreateAiHomeThread() {
  auto ensure = EnsureAiHomeThread();
  if (!ensure) {
    return ensure.error();
  }
  auto thread = store_.GetThread(ai_home_thread_id_);
  if (!thread || !*thread) {
    return Error("Failed to load AI home thread");
  }
  return OpenThread(ai_home_thread_id_);
}

Roe<Thread> InboxController::CreateNewAiThread() {
  auto ensure = EnsureAiHomeThread();
  if (!ensure) {
    return ensure.error();
  }

  Thread thread;
  thread.id = util::GenerateUuid();
  thread.kind = ThreadKind::Ai;
  thread.title = "New chat";
  thread.preview = "";
  thread.updated_at = util::NowUnixMs();

  auto saved = store_.UpsertThread(thread);
  if (!saved) {
    return saved.error();
  }
  return OpenThread(saved->id);
}

bool InboxController::IsAiHomeThread(const std::string& thread_id) const {
  return !ai_home_thread_id_.empty() && thread_id == ai_home_thread_id_;
}

Roe<void> InboxController::CloseThread(const std::string& thread_id) {
  if (IsAiHomeThread(thread_id)) {
    return Error("Cannot close AI home thread");
  }

  auto thread = store_.GetThread(thread_id);
  if (!thread) {
    return thread.error();
  }
  if (!*thread) {
    return Error("Thread not found");
  }

  const bool was_active = active_thread_id_ == thread_id;
  auto deleted = store_.DeleteThread(thread_id);
  if (!deleted || !*deleted) {
    return Error("Failed to delete thread");
  }

  if (was_active) {
    if (!ai_home_thread_id_.empty()) {
      if (auto opened = OpenThread(ai_home_thread_id_)) {
        return {};
      }
    }
    auto threads = store_.ListThreads();
    if (!threads || threads->empty()) {
      active_thread_id_.clear();
      if (on_thread_changed_) {
        on_thread_changed_();
      }
      return {};
    }
    (void)OpenThread(threads->front().id);
  } else if (on_thread_changed_) {
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
    return Error("Contact has no relay identity");
  }

  const std::string title =
      (*contact)->display_name.empty() ? (*contact)->server_nickname : (*contact)->display_name;
  auto thread = store_.FindOrCreateDirectThread(target, contact_id, title);
  if (!thread) {
    return thread.error();
  }
  return OpenThread(thread->id);
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
    return Error("Contact has no relay identity");
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
  Thread updated = **thread;
  updated.unread_count = 0;
  (void)store_.UpsertThread(updated);
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

void InboxController::SetOnThreadChanged(ThreadChangedCallback callback) {
  on_thread_changed_ = std::move(callback);
}

std::string InboxController::ResolveSenderLabel(const std::string& sender_contact_id) const {
  if (sender_contact_id == kLocalSelfContactId) {
    return "You";
  }
  if (sender_contact_id == kAiAssistantContactId) {
    return "AI";
  }
  if (auto contact = contacts_.Get(sender_contact_id)) {
    if (*contact) {
      return (*contact)->display_name.empty() ? (*contact)->server_nickname : (*contact)->display_name;
    }
  }
  return sender_contact_id;
}

std::string InboxController::ResolveRowClass(const std::string& sender_contact_id) const {
  if (sender_contact_id == kLocalSelfContactId) {
    return "message-row-user";
  }
  if (sender_contact_id == kAiAssistantContactId) {
    return "message-row-ai";
  }
  return "message-row-peer";
}

std::string InboxController::BuildMessageRml(const ThreadMessage& message) const {
  if (message.content_rml) {
    if (message.content_rml->find("__ENTRY__") != std::string::npos) {
      return InjectEntryPlaceholders(*message.content_rml, message.id);
    }
    return *message.content_rml;
  }
  const std::string bubble_class = message.sender_contact_id == kLocalSelfContactId ? "bubble-user" : "bubble-assistant";
  const std::string paragraph =
      message.sender_contact_id == kLocalSelfContactId ? "<p class=\"bubble-text\">" : "<p>";
  return "<div class=\"bubble " + bubble_class + "\" selectable=\"text\">" + paragraph +
         StructuredTextParser::EscapeText(message.text) + "</p></div>";
}

std::vector<MessageDisplayRow> InboxController::BuildDisplayRows(const std::string& thread_id) const {
  std::vector<MessageDisplayRow> rows;
  auto messages = store_.GetMessagesPage(thread_id, std::nullopt, kDefaultMessagesPageSize);
  if (!messages) {
    return rows;
  }

  for (const ThreadMessage& message : *messages) {
    MessageDisplayRow row;
    row.sender_label = ResolveSenderLabel(message.sender_contact_id).c_str();
    row.content_rml = BuildMessageRml(message).c_str();
    row.row_class = ResolveRowClass(message.sender_contact_id).c_str();
    row.has_content = true;
    rows.push_back(std::move(row));
  }
  return rows;
}

} // namespace pbr
