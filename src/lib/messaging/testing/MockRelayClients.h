#pragma once

// Messaging's relay and peer-history ports, mocked for tests (never linked into the app or pp-node).

#include "lib/messaging/RelayClients.h"

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

class MockRelayClient : public IRelayClient {
public:
  using BuildSignBytesFn = std::function<Roe<std::vector<uint8_t>>(const RelayEnvelope&)>;

  void SetNextReplySenderId(std::string sender_id) { next_reply_sender_id_ = std::move(sender_id); }
  void SetReplySigningPrivateKey(std::vector<uint8_t> private_key) {
    reply_signing_private_key_ = std::move(private_key);
  }
  /** Optional: wire EnvelopeSigner::BuildSignBytes from messaging tests without net→messaging. */
  void SetBuildSignBytesFn(BuildSignBytesFn fn) { build_sign_bytes_ = std::move(fn); }
  void AddDeliveredEnvelope(RelayEnvelope envelope) {
    std::lock_guard lock(mutex_);
    delivered_.push_back(std::move(envelope));
  }
  /** When set, FetchChatHistory returns this error (e.g. simulates relay 403). */
  void SetFetchHistoryError(std::string error) { fetch_history_error_ = std::move(error); }

  Roe<void> Send(const RelayEnvelope& envelope) override;
  Roe<RelayPollResult> PollInbox(const std::string& requester_contact_id, const std::string& cursor) override;
  Roe<RelayDeleteResult> AckInbox(const std::string& requester_contact_id, const std::string& cursor) override;
  Roe<RelayDeleteResult> ClearInbox(const std::string& requester_contact_id,
                                    const std::string& before_created_at) override;
  Roe<ChatHistoryResponse> FetchChatHistory(const ChatHistoryRequest& request) override;

private:
  std::mutex mutex_;
  std::vector<RelayEnvelope> pending_;
  std::vector<RelayEnvelope> delivered_;
  size_t poll_index_ = 0;
  std::string next_reply_sender_id_;
  std::vector<uint8_t> reply_signing_private_key_;
  BuildSignBytesFn build_sign_bytes_;
  std::string fetch_history_error_;
};

/** Test double for D060 peer-direct fetch. */
class MockChatHistoryPeerClient : public IChatHistoryPeerClient {
public:
  void SetPeerReachable(const std::string& peer_identity_value, const bool reachable = true) {
    std::lock_guard lock(mutex_);
    reachable_peers_[peer_identity_value] = reachable;
  }
  void AddDeliveredEnvelope(RelayEnvelope envelope) {
    std::lock_guard lock(mutex_);
    delivered_.push_back(std::move(envelope));
  }

  bool IsPeerReachable(const std::string& peer_identity_value) const override;
  Roe<ChatHistoryResponse> FetchChatHistory(const ChatHistoryRequest& request) override;

private:
  mutable std::mutex mutex_;
  std::unordered_map<std::string, bool> reachable_peers_;
  std::vector<RelayEnvelope> delivered_;
};

} // namespace pbr
