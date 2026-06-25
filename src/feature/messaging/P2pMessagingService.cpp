#include "base/people/ContactTypes.h"
#include "feature/messaging/P2pMessagingService.h"

#include "common/Utilities.h"
#include "base/messaging/MessagingJson.h"
#include "base/net/McpRelayClient.h"
#include "base/platform/BrowserThread.h"

#include <atomic>
#include <mutex>

namespace pbr {

namespace {

constexpr uint64_t kMcpRelayPollIntervalMs = 5000;

} // namespace

P2pMessagingService::P2pMessagingService(IThreadStore& store, ContactsStore& contacts, IdentityStore& identity,
                                         IRelayClient* relay, InboxController& inbox)
    : store_(store), contacts_(contacts), identity_(identity), relay_(relay), inbox_(inbox) {
  redirectLogger("P2pMessagingService");
  mcp_throttled_poll_ = dynamic_cast<McpRelayClient*>(relay_) != nullptr;
}

void P2pMessagingService::SetRelayClient(IRelayClient* relay) {
  relay_ = relay;
  relay_cursor_.clear();
  mcp_throttled_poll_ = dynamic_cast<McpRelayClient*>(relay_) != nullptr;
  poll_pending_ = false;
}

std::optional<std::string> P2pMessagingService::ResolvePeerRelayId(const Thread& thread) const {
  if (thread.participant_contact_ids.empty()) {
    return std::nullopt;
  }
  const auto contact = contacts_.Get(thread.participant_contact_ids.front());
  if (!contact || !*contact) {
    return std::nullopt;
  }
  for (const ContactId& id : (*contact)->ids) {
    if (id.kind == ContactIdKind::RelayUser) {
      return id.value;
    }
  }
  return std::nullopt;
}

TrustLevel P2pMessagingService::ResolveThreadTrust(const Thread& thread) const {
  if (thread.participant_contact_ids.empty()) {
    return TrustLevel::Unknown;
  }
  const auto contact = contacts_.Get(thread.participant_contact_ids.front());
  if (!contact || !*contact) {
    return TrustLevel::Unknown;
  }
  return (*contact)->trust;
}

void P2pMessagingService::EnqueueRetry(PendingRelaySend pending) {
  std::lock_guard lock(retry_mutex_);
  retry_queue_.push_back(std::move(pending));
}

void P2pMessagingService::NotifyDeliveryIssue(const Thread& thread, const std::string& error_message) {
  if (!on_delivery_notice_) {
    return;
  }
  const bool rate_limited = error_message.find("rate limit") != std::string::npos;
  std::string notice = error_message;
  if (rate_limited) {
    if (ResolveThreadTrust(thread) == TrustLevel::Friendly) {
      notice = "Relay is busy — your message will retry shortly.";
    } else {
      notice = "Relay rate limit — slow down messaging with new contacts.";
    }
  }
  BrowserThread::PostTask(BrowserThreadId::UI, [this, notice]() {
    if (on_delivery_notice_) {
      on_delivery_notice_(notice);
    }
  });
}

void P2pMessagingService::ApplySendResult(const std::string& thread_id, const std::string& message_id, bool success,
                                          const std::string& error_message) {
  auto messages = store_.GetMessages(thread_id);
  if (!messages) {
    return;
  }
  for (ThreadMessage& existing : *messages) {
    if (existing.id == message_id) {
      existing.delivery = success ? MessageDelivery::Relayed : MessageDelivery::Failed;
      (void)store_.UpdateMessage(existing);
      break;
    }
  }
  if (!success) {
    auto thread = store_.GetThread(thread_id);
    if (thread && *thread) {
      NotifyDeliveryIssue(**thread, error_message);
    }
  }
  if (on_messages_changed_) {
    BrowserThread::PostTask(BrowserThreadId::UI, [this]() { on_messages_changed_(); });
  }
}

Roe<ThreadMessage> P2pMessagingService::SendUserMessage(const std::string& thread_id, const std::string& text) {
  auto thread = store_.GetThread(thread_id);
  if (!thread) {
    return thread.error();
  }
  if (!*thread || (*thread)->kind != ThreadKind::Direct) {
    return Error("Not a direct thread");
  }

  ThreadMessage message;
  message.id = util::GenerateUuid();
  message.thread_id = thread_id;
  message.sender_contact_id = kLocalSelfContactId;
  message.text = text;
  message.timestamp = util::NowUnixMs();
  message.delivery = MessageDelivery::Pending;
  message.relay_visible = true;

  auto appended = store_.AppendMessage(message);
  if (!appended) {
    return appended.error();
  }

  (void)inbox_.UpdatePreview(thread_id, text);

  auto identity = identity_.Get();
  if (!identity) {
    appended->delivery = MessageDelivery::Failed;
    (void)store_.UpdateMessage(*appended);
    return appended.error();
  }

  RelayEnvelope envelope;
  envelope.thread_id = thread_id;
  envelope.message_id = message.id;
  envelope.sender_relay_id = identity->relay_user_id;
  envelope.body.text = text;
  envelope.timestamp = message.timestamp;

  auto signature = identity_.SignPayload(RelayEnvelopeToJson(envelope).dump());
  if (signature) {
    envelope.signature = *signature;
  }

  BrowserThread::PostTask(BrowserThreadId::IO, [this, envelope, message_id = message.id]() mutable {
    if (!relay_) {
      ApplySendResult(envelope.thread_id, message_id, false, "Relay client not configured");
      return;
    }
    const auto result = relay_->Send(envelope);
    if (!result) {
      EnqueueRetry(PendingRelaySend{.envelope = envelope, .message_id = message_id, .attempt_count = 1});
      ApplySendResult(envelope.thread_id, message_id, false, result.error().message);
      return;
    }
    ApplySendResult(envelope.thread_id, message_id, true);
  });

  if (on_messages_changed_) {
    on_messages_changed_();
  }
  return *appended;
}

void P2pMessagingService::RetryFailedOutbound() {
  std::vector<PendingRelaySend> pending;
  {
    std::lock_guard lock(retry_mutex_);
    pending.swap(retry_queue_);
  }
  if (pending.empty()) {
    return;
  }

  BrowserThread::PostTask(BrowserThreadId::IO, [this, pending = std::move(pending)]() mutable {
    if (!relay_) {
      std::lock_guard lock(retry_mutex_);
      retry_queue_.insert(retry_queue_.end(), std::make_move_iterator(pending.begin()),
                          std::make_move_iterator(pending.end()));
      return;
    }
    std::vector<PendingRelaySend> still_pending;
    for (PendingRelaySend& item : pending) {
      const auto result = relay_->Send(item.envelope);
      if (result) {
        ApplySendResult(item.envelope.thread_id, item.message_id, true);
        continue;
      }
      if (item.attempt_count < 5) {
        item.attempt_count += 1;
        still_pending.push_back(std::move(item));
        continue;
      }
      ApplySendResult(item.envelope.thread_id, item.message_id, false, result.error().message);
    }
    if (!still_pending.empty()) {
      std::lock_guard lock(retry_mutex_);
      retry_queue_.insert(retry_queue_.end(), std::make_move_iterator(still_pending.begin()),
                          std::make_move_iterator(still_pending.end()));
    }
  });
}

void P2pMessagingService::PollAndMerge() {
  RetryFailedOutbound();

  if (mcp_throttled_poll_) {
    const uint64_t now = util::NowUnixMs();
    if (now - last_relay_poll_ms_ < kMcpRelayPollIntervalMs) {
      return;
    }
    last_relay_poll_ms_ = now;
  }

  bool expected = false;
  if (!poll_pending_.compare_exchange_strong(expected, true)) {
    return;
  }

  BrowserThread::PostTask(BrowserThreadId::IO, [this]() {
    struct PollGuard {
      std::atomic<bool>& pending;
      ~PollGuard() { pending = false; }
    } guard{poll_pending_};

    if (!relay_) {
      return;
    }
    auto poll = relay_->PollInbox(relay_cursor_);
    if (!poll) {
      return;
    }
    relay_cursor_ = poll->next_cursor;

    bool changed = false;
    for (const RelayEnvelope& envelope : poll->messages) {
      if (store_.HasMessageId(envelope.message_id)) {
        continue;
      }

      ThreadMessage message;
      message.id = envelope.message_id;
      message.thread_id = envelope.thread_id;
      message.sender_contact_id = envelope.sender_relay_id;
      message.text = envelope.body.text;
      message.content_rml = envelope.body.content_rml;
      message.timestamp = envelope.timestamp;
      message.delivery = MessageDelivery::Relayed;
      message.relay_visible = true;

      auto thread = store_.GetThread(envelope.thread_id);
      if (thread && *thread && !(*thread)->participant_contact_ids.empty()) {
        message.sender_contact_id = (*thread)->participant_contact_ids.front();
      }

      if (store_.AppendMessage(message)) {
        changed = true;
        if (inbox_.ActiveThreadId() != envelope.thread_id) {
          if (thread && *thread) {
            Thread updated = **thread;
            updated.unread_count += 1;
            updated.preview = envelope.body.text;
            updated.updated_at = util::NowUnixMs();
            (void)store_.UpsertThread(updated);
          }
        } else {
          (void)inbox_.UpdatePreview(envelope.thread_id, envelope.body.text);
        }
      }
    }

    if (changed && on_messages_changed_) {
      BrowserThread::PostTask(BrowserThreadId::UI, [this]() { on_messages_changed_(); });
    }
  });
}

void P2pMessagingService::SetOnMessagesChanged(std::function<void()> callback) {
  on_messages_changed_ = std::move(callback);
}

void P2pMessagingService::SetOnDeliveryNotice(std::function<void(const std::string&)> callback) {
  on_delivery_notice_ = std::move(callback);
}

} // namespace pbr
