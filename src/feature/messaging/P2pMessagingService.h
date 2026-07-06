#pragma once

#include "common/Module.h"
#include "base/people/ContactsStore.h"
#include "base/people/IdentityStore.h"
#include "base/messaging/IThreadStore.h"
#include "base/crypto/IPskSessionStore.h"
#include "base/messaging/PeerSigningKeyStore.h"
#include "feature/messaging/ChatSyncService.h"
#include "feature/messaging/EpochBumpCoordinator.h"
#include "feature/messaging/InboxController.h"
#include "feature/messaging/Libp2pChatHistoryService.h"
#include "feature/messaging/PskSessionCoordinator.h"
#include "feature/messaging/RelayReceivePipeline.h"
#include "base/net/ServiceClients.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace pbr {

class P2pMessagingService : public Module {
public:
  P2pMessagingService(IThreadStore& store, ContactsStore& contacts, IdentityStore& identity, IRelayClient* relay,
                      InboxController& inbox, PeerSigningKeyStore& signing_key_store,
                      IPeerSigningKeyResolver& signing_key_resolver, IPskSessionStore& psk_store);

  Roe<ThreadMessage> SendUserMessage(const std::string& thread_id, const std::string& text);
  void PollAndMerge();
  void RetryFailedOutbound();
  void SetRelayClient(IRelayClient* relay);
  void SetOnMessagesChanged(std::function<void()> callback);
  void SetOnDeliveryNotice(std::function<void(const std::string&)> callback);
  void RegisterPeerSigningKey(const std::string& peer_identity_kind, const std::string& peer_identity_value,
                              const std::string& signing_public_key_b64, const std::string& source = "manual");
  void MaybeTailSync(const std::string& thread_id);
  /** D059 — full user-initiated sync (async on IO thread). */
  void SyncWithPeer(const std::string& thread_id, std::function<void(Roe<ChatSyncResult>)> on_complete = {});
  /** D059 — gap banner retry (async on IO thread). */
  void RetryGapSync(const std::string& thread_id, std::function<void(Roe<ChatSyncResult>)> on_complete = {});
  /** D038/D068 — epoch-only bump to recover from compromised state. */
  Roe<uint32_t> StartNewSecureChat(const std::string& thread_id);
  /** E020 — rotate PSK, bump epoch, return OOB bundle JSON. */
  Roe<std::string> RotatePskAndExportBundle(const std::string& thread_id);
  /** D038 — pause ingest/outbound without rotating keys. */
  Roe<void> PauseIntegrityOnly(const std::string& thread_id);

  Roe<PskSessionStatus> GetPskStatus(const std::string& thread_id) const;
  Roe<PskExportView> EnsurePskGenerated(const std::string& thread_id);
  Roe<PskExportView> GetPskExportView(const std::string& thread_id) const;
  Roe<std::string> ExportPskBundleJson(const std::string& thread_id) const;
  Roe<void> ImportPskRawBase64(const std::string& thread_id, const std::string& raw_b64);
  Roe<void> ImportPskBundleJson(const std::string& thread_id, const std::string& bundle_json);
  Roe<void> MarkPskVerified(const std::string& thread_id);
  void RegisterPeerDirectEndpoint(const std::string& peer_relay_user_id, const std::string& multiaddr);
  void TailSyncActiveE2eThread();

private:
  struct PendingRelaySend {
    RelayEnvelope envelope;
    std::string message_id;
    std::string thread_id;
    int attempt_count = 0;
  };

  std::optional<std::string> ResolvePeerRelayId(const Thread& thread) const;
  TrustLevel ResolveThreadTrust(const Thread& thread) const;
  void EnqueueRetry(PendingRelaySend pending);
  void NotifyDeliveryIssue(const Thread& thread, const std::string& error_message);
  void ApplySendResult(const std::string& thread_id, const std::string& message_id, bool success,
                       const std::string& error_message = {});
  void RegisterMockPeerKeyForReply(const std::string& peer_identity_value);
  void MaybeRepairGap(const std::string& thread_id, const RelayEnvelope& envelope);
  void RunSyncOnIo(const std::string& thread_id, std::function<Roe<ChatSyncResult>()> task,
                   std::function<void(Roe<ChatSyncResult>)> on_complete);
  bool IsE2ePrivateThread(const std::string& thread_id) const;
  bool IsThreadCompromised(const std::string& thread_id) const;
  bool IsPskReadyToSend(const std::string& thread_id) const;
  void PurgeRetryQueueForThread(const std::string& thread_id);

  IThreadStore& store_;
  ContactsStore& contacts_;
  IdentityStore& identity_;
  IRelayClient* relay_ = nullptr;
  InboxController& inbox_;
  PeerSigningKeyStore& signing_key_store_;
  IPeerSigningKeyResolver& signing_key_resolver_;
  IPskSessionStore& psk_store_;
  std::unique_ptr<RelayReceivePipeline> receive_pipeline_;
  std::unique_ptr<Libp2pChatHistoryService> peer_history_;
  std::unique_ptr<ChatSyncService> chat_sync_;
  EpochBumpCoordinator epoch_coordinator_;
  PskSessionCoordinator psk_coordinator_;
  std::string relay_cursor_;
  std::function<void()> on_messages_changed_;
  std::function<void(const std::string&)> on_delivery_notice_;
  mutable std::mutex retry_mutex_;
  std::vector<PendingRelaySend> retry_queue_;
  uint64_t last_relay_poll_ms_ = 0;
  std::atomic<bool> poll_pending_{false};
  std::atomic<bool> sync_pending_{false};
};

} // namespace pbr
