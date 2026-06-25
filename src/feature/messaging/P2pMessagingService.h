#pragma once

#include "common/Module.h"
#include "contacts/ContactsStore.h"
#include "identity/IdentityStore.h"
#include "messaging/IThreadStore.h"
#include "messaging/InboxController.h"
#include "net/ServiceClients.h"

#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace pbr {

class P2pMessagingService : public Module {
public:
  P2pMessagingService(IThreadStore& store, ContactsStore& contacts, IdentityStore& identity, IRelayClient& relay,
                      InboxController& inbox);

  Roe<ThreadMessage> SendUserMessage(const std::string& thread_id, const std::string& text);
  void PollAndMerge();
  void RetryFailedOutbound();
  void SetOnMessagesChanged(std::function<void()> callback);
  void SetOnDeliveryNotice(std::function<void(const std::string&)> callback);

private:
  struct PendingRelaySend {
    RelayEnvelope envelope;
    std::string message_id;
    int attempt_count = 0;
  };

  std::optional<std::string> ResolvePeerRelayId(const Thread& thread) const;
  TrustLevel ResolveThreadTrust(const Thread& thread) const;
  void EnqueueRetry(PendingRelaySend pending);
  void NotifyDeliveryIssue(const Thread& thread, const std::string& error_message);
  void ApplySendResult(const std::string& thread_id, const std::string& message_id, bool success,
                       const std::string& error_message = {});

  IThreadStore& store_;
  ContactsStore& contacts_;
  IdentityStore& identity_;
  IRelayClient& relay_;
  InboxController& inbox_;
  std::string relay_cursor_;
  std::function<void()> on_messages_changed_;
  std::function<void(const std::string&)> on_delivery_notice_;
  mutable std::mutex retry_mutex_;
  std::vector<PendingRelaySend> retry_queue_;
};

} // namespace pbr
