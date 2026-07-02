#include "base/people/ContactTypes.h"
#include "feature/messaging/P2pMessagingService.h"

#include "common/Utilities.h"
#include "base/messaging/EnvelopeSigner.h"
#include "base/messaging/MessagingJson.h"
#include "base/messaging/MessagingLimits.h"
#include "base/messaging/RelayWirePayload.h"
#include "base/net/McpRelayClient.h"
#include "base/net/ServiceClientsImpl.h"
#include "base/platform/BrowserThread.h"

#include <atomic>
#include <mutex>

#include <nlohmann/json.hpp>

namespace pbr {

namespace {

constexpr const char* kMockPeerSigningPublicKeyB64 = "A6EHv/POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMbg=";

DirectChatTarget InboundTargetFromEnvelope(const RelayEnvelope& envelope) {
  DirectChatTarget target;
  target.peer_identity_kind = ContactIdKindToString(ContactIdKind::RelayUser);
  target.peer_identity_value = envelope.sender_contact_id;
  target.channel = envelope.route.channel;
  return target;
}

} // namespace

P2pMessagingService::P2pMessagingService(IThreadStore& store, ContactsStore& contacts, IdentityStore& identity,
                                         IRelayClient* relay, InboxController& inbox,
                                         PeerSigningKeyStore& signing_key_store)
    : store_(store), contacts_(contacts), identity_(identity), relay_(relay), inbox_(inbox),
      signing_key_store_(signing_key_store), signing_key_resolver_(signing_key_store_) {
  redirectLogger("P2pMessagingService");
  receive_pipeline_ = std::make_unique<RelayReceivePipeline>(store_, signing_key_resolver_);
  chat_sync_ = std::make_unique<ChatSyncService>(store_, identity_, relay_, *receive_pipeline_);
  chat_sync_->SetOnMessagesChanged([this]() {
    if (on_messages_changed_) {
      BrowserThread::PostTask(BrowserThreadId::UI, [this]() { on_messages_changed_(); });
    }
  });
  mcp_throttled_poll_ = dynamic_cast<McpRelayClient*>(relay_) != nullptr;
}

void P2pMessagingService::RegisterPeerSigningKey(const std::string& peer_identity_kind,
                                               const std::string& peer_identity_value,
                                               const std::string& signing_public_key_b64, const std::string& source) {
  PeerSigningKeyRecord record;
  record.signing_public_key_b64 = signing_public_key_b64;
  record.source = source;
  signing_key_store_.Put(peer_identity_kind, peer_identity_value, std::move(record));
}

void P2pMessagingService::RegisterMockPeerKeyForReply(const std::string& peer_identity_value) {
  RegisterPeerSigningKey(ContactIdKindToString(ContactIdKind::RelayUser), peer_identity_value,
                         kMockPeerSigningPublicKeyB64, "mock_relay");
}

void P2pMessagingService::SetRelayClient(IRelayClient* relay) {
  relay_ = relay;
  relay_cursor_.clear();
  mcp_throttled_poll_ = dynamic_cast<McpRelayClient*>(relay_) != nullptr;
  poll_pending_ = false;
  if (chat_sync_) {
    chat_sync_ = std::make_unique<ChatSyncService>(store_, identity_, relay_, *receive_pipeline_);
    chat_sync_->SetOnMessagesChanged([this]() {
      if (on_messages_changed_) {
        BrowserThread::PostTask(BrowserThreadId::UI, [this]() { on_messages_changed_(); });
      }
    });
  }
}

std::optional<std::string> P2pMessagingService::ResolvePeerRelayId(const Thread& thread) const {
  if (!thread.peer_identity_value.empty()) {
    return thread.peer_identity_value;
  }
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

void P2pMessagingService::MaybeTailSync(const std::string& thread_id) {
  if (!chat_sync_) {
    return;
  }
  BrowserThread::PostTask(BrowserThreadId::IO, [this, thread_id]() { (void)chat_sync_->TailSync(thread_id); });
}

void P2pMessagingService::MaybeRepairGap(const std::string& thread_id, const RelayEnvelope& envelope) {
  if (!chat_sync_ || !envelope.session_epoch) {
    return;
  }
  auto sync_state = store_.GetPeerSyncState(thread_id, envelope.session_epoch);
  if (!sync_state || sync_state->phase == PeerSyncPhase::Compromised) {
    return;
  }
  if (envelope.sender_seq <= sync_state->contiguous_peer_seq + 1) {
    return;
  }
  const uint64_t gap_min = sync_state->contiguous_peer_seq + 1;
  const uint64_t gap_max = envelope.sender_seq - 1;
  (void)chat_sync_->RepairGap(thread_id, gap_min, gap_max);
}

void P2pMessagingService::ApplySendResult(const std::string& thread_id, const std::string& message_id, bool success,
                                          const std::string& error_message) {
  auto messages = store_.GetMessagesPage(thread_id, std::nullopt, 10000);
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
  if ((*thread)->channel == ThreadChannel::E2ePublic) {
    return Error("Public tier messaging is not enabled yet");
  }
  if ((*thread)->peer_identity_value.empty()) {
    return Error("Direct thread missing peer identity");
  }

  ThreadMessage message;
  message.id = util::GenerateUuid();
  message.thread_id = thread_id;
  message.sender_contact_id = kLocalSelfContactId;
  message.text = text;
  message.timestamp = util::NowUnixMs();
  message.delivery = MessageDelivery::Pending;
  message.relay_visible = true;
  message.transport = MessageTransport::Relay;

  auto sender_seq = store_.AllocateSenderSeq(thread_id);
  if (!sender_seq) {
    return sender_seq.error();
  }
  auto session_epoch = store_.GetChatTargetSessionEpoch(thread_id);
  if (!session_epoch) {
    return session_epoch.error();
  }
  message.sender_seq = *sender_seq;
  message.session_epoch = *session_epoch;

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

  auto payload_b64 = RelayWirePayload::EncodePlaintextText(text);
  if (!payload_b64) {
    appended->delivery = MessageDelivery::Failed;
    (void)store_.UpdateMessage(*appended);
    return payload_b64.error();
  }

  RelayEnvelope envelope;
  envelope.envelope_version = kRelayEnvelopeVersion;
  envelope.message_id = message.id;
  envelope.sender_relay_id = identity->relay_user_id;
  envelope.sender_contact_id = identity->relay_user_id;
  envelope.route.kind = "direct";
  envelope.route.channel = (*thread)->channel;
  envelope.body.e2e.payload_b64 = *payload_b64;
  envelope.sender_seq = *message.sender_seq;
  envelope.session_epoch = *message.session_epoch;
  envelope.timestamp = message.timestamp;

  auto sign_bytes = EnvelopeSigner::BuildSignBytes(envelope);
  if (!sign_bytes) {
    appended->delivery = MessageDelivery::Failed;
    (void)store_.UpdateMessage(*appended);
    return sign_bytes.error();
  }
  auto signature = identity_.SignBytes(*sign_bytes);
  if (signature) {
    envelope.signature = *signature;
  }

  if (auto* mock_relay = dynamic_cast<MockRelayClient*>(relay_)) {
    mock_relay->SetNextReplySenderId((*thread)->peer_identity_value);
    RegisterMockPeerKeyForReply((*thread)->peer_identity_value);
  }

  BrowserThread::PostTask(BrowserThreadId::IO, [this, thread_id, envelope, message_id = message.id]() mutable {
    if (!relay_) {
      ApplySendResult(thread_id, message_id, false, "Relay client not configured");
      return;
    }
    const auto result = relay_->Send(envelope);
    if (!result) {
      EnqueueRetry(PendingRelaySend{.envelope = envelope, .message_id = message_id, .thread_id = thread_id,
                                    .attempt_count = 1});
      ApplySendResult(thread_id, message_id, false, result.error().message);
      return;
    }
    ApplySendResult(thread_id, message_id, true);
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
        ApplySendResult(item.thread_id, item.message_id, true);
        continue;
      }
      if (item.attempt_count < kMaxOutboxRetryAttempts) {
        item.attempt_count += 1;
        still_pending.push_back(std::move(item));
        continue;
      }
      ApplySendResult(item.thread_id, item.message_id, false, result.error().message);
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

  const uint64_t now = util::NowUnixMs();
  const uint64_t poll_interval = mcp_throttled_poll_ ? 5000 : kForegroundRelayPollIntervalMs;
  if (now - last_relay_poll_ms_ < poll_interval) {
    return;
  }
  last_relay_poll_ms_ = now;

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
    if (poll->messages.size() > kMaxPollBatchMessages) {
      return;
    }

    bool changed = false;
    for (const RelayEnvelope& envelope : poll->messages) {
      const RelayReceiveOutcome outcome = receive_pipeline_->ProcessEnvelope(envelope);
      if (outcome.decision == IngestDecision::AcceptGap) {
        const DirectChatTarget inbound_target = InboundTargetFromEnvelope(envelope);
        auto thread = store_.FindDirectThread(inbound_target);
        if (thread && *thread) {
          MaybeRepairGap((*thread)->id, envelope);
        }
      }
      if (!outcome.persisted) {
        continue;
      }
      changed = true;
      const DirectChatTarget inbound_target = InboundTargetFromEnvelope(envelope);
      auto thread = store_.FindDirectThread(inbound_target);
      if (!thread || !*thread) {
        continue;
      }
      const std::string& resolved_thread_id = (*thread)->id;
      if (inbox_.ActiveThreadId() != resolved_thread_id) {
        Thread updated = **thread;
        updated.unread_count += 1;
        auto decoded = RelayWirePayload::DecodeInboundPayload(envelope.body.e2e.payload_b64);
        if (decoded) {
          updated.preview = decoded->text;
        }
        updated.updated_at = util::NowUnixMs();
        (void)store_.UpsertThread(updated);
      } else if (auto decoded = RelayWirePayload::DecodeInboundPayload(envelope.body.e2e.payload_b64); decoded) {
        (void)inbox_.UpdatePreview(resolved_thread_id, decoded->text);
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
