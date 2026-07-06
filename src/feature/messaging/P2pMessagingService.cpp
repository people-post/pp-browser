#include "base/people/ContactTypes.h"
#include "feature/messaging/Libp2pChatHistoryService.h"
#include "feature/messaging/P2pMessagingService.h"

#include "base/crypto/CryptoUtil.h"
#include "common/Utilities.h"
#include "base/messaging/E2eIntegrityUtil.h"
#include "base/messaging/E2eRelayPayloadCodec.h"
#include "base/messaging/EnvelopeSigner.h"
#include "base/messaging/MessagingJson.h"
#include "base/messaging/MessagingLimits.h"
#include "base/messaging/RelayStreamKey.h"
#include "base/messaging/RelayWirePayload.h"
#include "base/messaging/SyncStateTypes.h"
#include "base/net/ServiceClientsImpl.h"
#include "base/platform/BrowserThread.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <optional>

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
                                         PeerSigningKeyStore& signing_key_store,
                                         IPeerSigningKeyResolver& signing_key_resolver, IPskSessionStore& psk_store)
    : store_(store), contacts_(contacts), identity_(identity), relay_(relay), inbox_(inbox),
      signing_key_store_(signing_key_store), signing_key_resolver_(signing_key_resolver), psk_store_(psk_store),
      epoch_coordinator_(store), psk_coordinator_(store, psk_store) {
  redirectLogger("P2pMessagingService");
  receive_pipeline_ = std::make_unique<RelayReceivePipeline>(store_, signing_key_resolver_, psk_store_);
  peer_history_ = std::make_unique<Libp2pChatHistoryService>(store_, identity_, psk_store_);
  peer_history_->Start();
  chat_sync_ = std::make_unique<ChatSyncService>(store_, identity_, relay_, *receive_pipeline_, peer_history_.get());
  chat_sync_->SetOnMessagesChanged([this]() {
    if (on_messages_changed_) {
      BrowserThread::PostTask(BrowserThreadId::UI, [this]() { on_messages_changed_(); });
    }
  });
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
  poll_pending_ = false;
  if (chat_sync_) {
    chat_sync_ = std::make_unique<ChatSyncService>(store_, identity_, relay_, *receive_pipeline_, peer_history_.get());
    chat_sync_->SetOnMessagesChanged([this]() {
      if (on_messages_changed_) {
        BrowserThread::PostTask(BrowserThreadId::UI, [this]() { on_messages_changed_(); });
      }
    });
  }
  TailSyncActiveE2eThread();
}

void P2pMessagingService::RegisterPeerDirectEndpoint(const std::string& peer_relay_user_id,
                                                     const std::string& multiaddr) {
  if (peer_history_) {
    peer_history_->RegisterPeerEndpoint(peer_relay_user_id, multiaddr);
  }
}

bool P2pMessagingService::IsE2ePrivateThread(const std::string& thread_id) const {
  auto thread = store_.GetThread(thread_id);
  if (!thread || !*thread) {
    return false;
  }
  return (*thread)->kind == ThreadKind::Direct && (*thread)->channel == ThreadChannel::E2e;
}

bool P2pMessagingService::IsThreadCompromised(const std::string& thread_id) const {
  return IsE2ePrivateThread(thread_id) && IsE2eThreadCompromised(store_, thread_id);
}

bool P2pMessagingService::IsPskReadyToSend(const std::string& thread_id) const {
  if (!IsE2ePrivateThread(thread_id)) {
    return true;
  }
  auto status = psk_coordinator_.GetStatus(thread_id);
  return status && status->has_psk && status->verified;
}

void P2pMessagingService::PurgeRetryQueueForThread(const std::string& thread_id) {
  std::lock_guard lock(retry_mutex_);
  retry_queue_.erase(std::remove_if(retry_queue_.begin(), retry_queue_.end(),
                                     [&](const PendingRelaySend& item) { return item.thread_id == thread_id; }),
                     retry_queue_.end());
}

Roe<uint32_t> P2pMessagingService::StartNewSecureChat(const std::string& thread_id) {
  if (!IsE2ePrivateThread(thread_id)) {
    return Error("Not a private E2E thread");
  }
  auto new_epoch = epoch_coordinator_.StartNewSecureChat(thread_id);
  if (!new_epoch) {
    return new_epoch.error();
  }
  PurgeRetryQueueForThread(thread_id);
  if (on_messages_changed_) {
    BrowserThread::PostTask(BrowserThreadId::UI, [this]() { on_messages_changed_(); });
  }
  return *new_epoch;
}

Roe<void> P2pMessagingService::PauseIntegrityOnly(const std::string& thread_id) {
  if (!IsE2ePrivateThread(thread_id)) {
    return Error("Not a private E2E thread");
  }
  PurgeRetryQueueForThread(thread_id);
  return epoch_coordinator_.PauseOnly(thread_id);
}

Roe<std::string> P2pMessagingService::RotatePskAndExportBundle(const std::string& thread_id) {
  if (!IsE2ePrivateThread(thread_id)) {
    return Error("Not a private E2E thread");
  }
  auto bundle = psk_coordinator_.RotatePskAndExportBundle(thread_id, util::NowUnixMs());
  if (!bundle) {
    return bundle.error();
  }
  PurgeRetryQueueForThread(thread_id);
  if (on_messages_changed_) {
    BrowserThread::PostTask(BrowserThreadId::UI, [this]() { on_messages_changed_(); });
  }
  return bundle;
}

Roe<PskSessionStatus> P2pMessagingService::GetPskStatus(const std::string& thread_id) const {
  return psk_coordinator_.GetStatus(thread_id);
}

Roe<PskExportView> P2pMessagingService::EnsurePskGenerated(const std::string& thread_id) {
  return psk_coordinator_.EnsureGenerated(thread_id);
}

Roe<PskExportView> P2pMessagingService::GetPskExportView(const std::string& thread_id) const {
  return psk_coordinator_.GetExportView(thread_id);
}

Roe<std::string> P2pMessagingService::ExportPskBundleJson(const std::string& thread_id) const {
  return psk_coordinator_.ExportBundleJson(thread_id);
}

Roe<void> P2pMessagingService::ImportPskRawBase64(const std::string& thread_id, const std::string& raw_b64) {
  return psk_coordinator_.ImportRawBase64(thread_id, raw_b64);
}

Roe<void> P2pMessagingService::ImportPskBundleJson(const std::string& thread_id, const std::string& bundle_json) {
  return psk_coordinator_.ImportBundleJson(thread_id, bundle_json);
}

Roe<void> P2pMessagingService::MarkPskVerified(const std::string& thread_id) {
  return psk_coordinator_.MarkVerified(thread_id, util::NowUnixMs());
}

void P2pMessagingService::TailSyncActiveE2eThread() {
  const std::string& active_id = inbox_.ActiveThreadId();
  if (active_id.empty() || !IsE2ePrivateThread(active_id)) {
    return;
  }
  MaybeTailSync(active_id);
}

void P2pMessagingService::RunSyncOnIo(const std::string& thread_id,
                                      std::function<Roe<ChatSyncResult>()> task,
                                      std::function<void(Roe<ChatSyncResult>)> on_complete) {
  if (!chat_sync_ || !IsE2ePrivateThread(thread_id)) {
    if (on_complete) {
      BrowserThread::PostTask(BrowserThreadId::UI,
                              [on_complete = std::move(on_complete)]() { on_complete(Error("Sync not available")); });
    }
    return;
  }

  bool expected = false;
  if (!sync_pending_.compare_exchange_strong(expected, true)) {
    if (on_complete) {
      BrowserThread::PostTask(BrowserThreadId::UI, [on_complete = std::move(on_complete)]() {
        on_complete(Error("Sync already in progress"));
      });
    }
    return;
  }

  BrowserThread::PostTask(BrowserThreadId::IO, [this, thread_id, task = std::move(task),
                                                  on_complete = std::move(on_complete)]() mutable {
    struct SyncGuard {
      std::atomic<bool>& pending;
      ~SyncGuard() { pending = false; }
    } guard{sync_pending_};

    Roe<ChatSyncResult> result = task();
    if (result && on_messages_changed_) {
      BrowserThread::PostTask(BrowserThreadId::UI, [this]() { on_messages_changed_(); });
    }
    if (on_complete) {
      BrowserThread::PostTask(BrowserThreadId::UI,
                              [on_complete = std::move(on_complete), result = std::move(result)]() mutable {
                                on_complete(std::move(result));
                              });
    }
  });
}

void P2pMessagingService::SyncWithPeer(const std::string& thread_id,
                                       std::function<void(Roe<ChatSyncResult>)> on_complete) {
  RunSyncOnIo(thread_id, [this, thread_id]() { return chat_sync_->UserInitiatedSync(thread_id); },
              std::move(on_complete));
}

void P2pMessagingService::RetryGapSync(const std::string& thread_id,
                                       std::function<void(Roe<ChatSyncResult>)> on_complete) {
  RunSyncOnIo(thread_id, [this, thread_id]() { return chat_sync_->RetryGapSync(thread_id); }, std::move(on_complete));
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
  if ((*thread)->channel == ThreadChannel::E2e && IsThreadCompromised(thread_id)) {
    return Error("Messaging paused — rotate the encryption key or pause only");
  }
  if ((*thread)->channel == ThreadChannel::E2e && !IsPskReadyToSend(thread_id)) {
    return Error("Verify the encryption key with your contact before sending");
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

  auto peer_relay_id = ResolvePeerRelayId(**thread);
  if (!peer_relay_id) {
    appended->delivery = MessageDelivery::Failed;
    (void)store_.UpdateMessage(*appended);
    return Error("Direct thread missing peer relay id");
  }

  std::optional<std::string> payload_b64;
  if (E2eRelayPayloadCodec::RequiresEncryption((*thread)->channel)) {
    const ChatTargetKey target_key = E2eRelayPayloadCodec::ChatTargetFromThread(**thread);
    auto master_psk_b64 = psk_store_.ResolveMasterPskForEpoch(target_key, *message.session_epoch);
    if (!master_psk_b64 || !master_psk_b64->has_value()) {
      appended->delivery = MessageDelivery::Failed;
      (void)store_.UpdateMessage(*appended);
      return Error("PSK not configured for this chat");
    }
    auto master_psk = Base64Decode(**master_psk_b64);
    if (!master_psk) {
      appended->delivery = MessageDelivery::Failed;
      (void)store_.UpdateMessage(*appended);
      return master_psk.error();
    }
    E2eEncryptParams params;
    params.text = text;
    params.channel = E2eRelayPayloadCodec::ChannelFromThread((*thread)->channel);
    params.peer_contact_id = *peer_relay_id;
    params.sender_contact_id = identity->relay_user_id;
    params.message_id = message.id;
    params.sender_seq = *message.sender_seq;
    params.session_epoch = *message.session_epoch;
    params.timestamp = message.timestamp;
    auto encrypted = E2eRelayPayloadCodec::EncryptText(params, *master_psk);
    if (!encrypted) {
      appended->delivery = MessageDelivery::Failed;
      (void)store_.UpdateMessage(*appended);
      return encrypted.error();
    }
    payload_b64 = std::move(*encrypted);
  } else {
    auto plaintext = RelayWirePayload::EncodePlaintextText(text);
    if (!plaintext) {
      appended->delivery = MessageDelivery::Failed;
      (void)store_.UpdateMessage(*appended);
      return plaintext.error();
    }
    payload_b64 = std::move(*plaintext);
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
  envelope.order_key = envelope.sender_seq;
  envelope.session_epoch = *message.session_epoch;
  envelope.stream_key = BuildCanonicalRelayStreamKey(identity->relay_user_id, *peer_relay_id, (*thread)->channel,
                                                     *message.session_epoch);
  envelope.recipient_contact_id = *peer_relay_id;
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

  pending.erase(std::remove_if(pending.begin(), pending.end(),
                               [this](const PendingRelaySend& item) { return IsThreadCompromised(item.thread_id); }),
                pending.end());
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
      if (IsThreadCompromised(item.thread_id)) {
        continue;
      }
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
  const uint64_t poll_interval = kForegroundRelayPollIntervalMs;
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
    auto identity = identity_.Get();
    if (!identity) {
      return;
    }
    auto poll = relay_->PollInbox(identity->relay_user_id, relay_cursor_);
    if (!poll) {
      return;
    }
    relay_cursor_ = poll->next_cursor;
    if (poll->messages.size() > kMaxPollBatchMessages) {
      return;
    }

    bool changed = false;
    const std::string local_relay_id = identity->relay_user_id;
    for (const RelayEnvelope& envelope : poll->messages) {
      const RelayReceiveOutcome outcome = receive_pipeline_->ProcessEnvelope(envelope, local_relay_id);
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
