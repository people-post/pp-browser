#pragma once

#include "common/Module.h"
#include "base/people/ContactsStore.h"
#include "base/people/IdentityStore.h"
#include "base/messaging/IThreadStore.h"
#include "base/messaging/PeerSigningKeyStore.h"
#include "feature/messaging/ChatSyncService.h"
#include "feature/messaging/InboxController.h"
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
                      InboxController& inbox, PeerSigningKeyStore& signing_key_store);

  Roe<ThreadMessage> SendUserMessage(const std::string& thread_id, const std::string& text);
  void PollAndMerge();
  void RetryFailedOutbound();
  void SetRelayClient(IRelayClient* relay);
  void SetOnMessagesChanged(std::function<void()> callback);
  void SetOnDeliveryNotice(std::function<void(const std::string&)> callback);
  void RegisterPeerSigningKey(const std::string& peer_identity_kind, const std::string& peer_identity_value,
                              const std::string& signing_public_key_b64, const std::string& source = "manual");
  void MaybeTailSync(const std::string& thread_id);

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

  IThreadStore& store_;
  ContactsStore& contacts_;
  IdentityStore& identity_;
  IRelayClient* relay_ = nullptr;
  InboxController& inbox_;
  PeerSigningKeyStore& signing_key_store_;
  PeerSigningKeyResolver signing_key_resolver_;
  std::unique_ptr<RelayReceivePipeline> receive_pipeline_;
  std::unique_ptr<ChatSyncService> chat_sync_;
  std::string relay_cursor_;
  std::function<void()> on_messages_changed_;
  std::function<void(const std::string&)> on_delivery_notice_;
  mutable std::mutex retry_mutex_;
  std::vector<PendingRelaySend> retry_queue_;
  bool mcp_throttled_poll_ = false;
  uint64_t last_relay_poll_ms_ = 0;
  std::atomic<bool> poll_pending_{false};
};

} // namespace pbr
