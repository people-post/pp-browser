#pragma once

#include "base/people/ContactTypes.h"
#include "base/net/ServiceClients.h"

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace pbr {

using RelayAuthSigner = std::function<Roe<std::string>(const std::vector<uint8_t>&)>;

class MockDirectoryClient : public IDirectoryClient {
public:
  Roe<std::vector<DirectoryHit>> SearchPeople(const std::string& query) override;
  Roe<DirectoryHit> LookupRelayUser(const std::string& relay_user_id) override;
};

class MockRelayClient : public IRelayClient {
public:
  void SetNextReplySenderId(std::string sender_id) { next_reply_sender_id_ = std::move(sender_id); }
  void SetReplySigningPrivateKey(std::vector<uint8_t> private_key) {
    reply_signing_private_key_ = std::move(private_key);
  }
  void AddDeliveredEnvelope(RelayEnvelope envelope) {
    std::lock_guard lock(mutex_);
    delivered_.push_back(std::move(envelope));
  }
  /** When set, FetchChatHistory returns this error (e.g. simulates relay 403). */
  void SetFetchHistoryError(std::string error) { fetch_history_error_ = std::move(error); }

  Roe<void> Send(const RelayEnvelope& envelope) override;
  Roe<RelayPollResult> PollInbox(const std::string& requester_contact_id, const std::string& cursor) override;
  Roe<ChatHistoryResponse> FetchChatHistory(const ChatHistoryRequest& request) override;

private:
  std::mutex mutex_;
  std::vector<RelayEnvelope> pending_;
  std::vector<RelayEnvelope> delivered_;
  size_t poll_index_ = 0;
  std::string next_reply_sender_id_;
  std::vector<uint8_t> reply_signing_private_key_;
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

class MockRegistrationClient : public IRegistrationClient {
public:
  Roe<RegistrationStartResult> StartRegistration(const std::string& public_key_b64, const std::string& nickname,
                                                 const std::string& signature_alg = "ed25519") override;
  Roe<RegistrationResult> FinishRegistration(const std::string& challenge, const std::string& public_key_b64,
                                             const std::string& nickname, const std::string& signature,
                                             int64_t timestamp, const std::string& signature_alg = "ed25519") override;
  Roe<RegistrationResult> UpdateNickname(const std::string& new_nickname, const std::string& signature,
                                         int64_t timestamp, const std::string& relay_user_id) override;
};

class HttpRelayClient : public IRelayClient {
public:
  explicit HttpRelayClient(std::string base_url);
  void SetAuthSigner(RelayAuthSigner signer) { auth_signer_ = std::move(signer); }
  Roe<void> Send(const RelayEnvelope& envelope) override;
  Roe<RelayPollResult> PollInbox(const std::string& requester_contact_id, const std::string& cursor) override;
  Roe<ChatHistoryResponse> FetchChatHistory(const ChatHistoryRequest& request) override;

private:
  Roe<std::string> SignRelayApiBytes(const std::vector<uint8_t>& sign_bytes) const;

  std::string base_url_;
  RelayAuthSigner auth_signer_;
};

class HttpDirectoryClient : public IDirectoryClient {
public:
  explicit HttpDirectoryClient(std::string base_url);
  Roe<std::vector<DirectoryHit>> SearchPeople(const std::string& query) override;
  Roe<DirectoryHit> LookupRelayUser(const std::string& relay_user_id) override;

private:
  std::string base_url_;
};

class HttpRegistrationClient : public IRegistrationClient {
public:
  explicit HttpRegistrationClient(std::string base_url);
  Roe<RegistrationStartResult> StartRegistration(const std::string& public_key_b64, const std::string& nickname,
                                                 const std::string& signature_alg = "ed25519") override;
  Roe<RegistrationResult> FinishRegistration(const std::string& challenge, const std::string& public_key_b64,
                                             const std::string& nickname, const std::string& signature,
                                             int64_t timestamp, const std::string& signature_alg = "ed25519") override;
  Roe<RegistrationResult> UpdateNickname(const std::string& new_nickname, const std::string& signature,
                                         int64_t timestamp, const std::string& relay_user_id) override;

private:
  std::string base_url_;
};

} // namespace pbr
