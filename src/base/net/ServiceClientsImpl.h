#pragma once

#include "common/DirectoryTypes.h"
#include "base/net/BlobClient.h"
#include "base/net/ClientCompat.h"
#include "base/net/IPushDeviceClient.h"
#include "base/net/RelayApiSignPayload.h"
#include "base/net/ServiceClients.h"

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

class MockBlobClient : public IBlobClient {
public:
  Roe<BlobPresignResult> Presign(const std::string& relay_user_id, const std::string& content_type,
                                 uint64_t byte_length, BlobPurpose purpose) override;
  Roe<void> Retain(const std::string& relay_user_id, const std::string& blob_id) override;
  Roe<void> Delete(const std::string& relay_user_id, const std::string& blob_id) override;
  Roe<BlobListResult> List(const std::string& relay_user_id, const std::string& status_filter = "") override;
  Roe<void> SetProfileIcon(const std::string& relay_user_id, const std::string& url, const std::string& blob_id,
                           const std::string& kind) override;
  Roe<void> PutUpload(const std::string& upload_url, const std::string& content_type,
                      const std::string& body) override;

  void SetPresignError(Error error) { presign_error_ = std::move(error); }
  void ClearPresignError() { presign_error_ = std::nullopt; }
  void SetListResult(BlobListResult result) { list_result_ = std::move(result); }

  const std::vector<std::string>& UploadedBodies() const { return uploaded_bodies_; }
  const std::vector<std::string>& RetainedBlobIds() const { return retained_blob_ids_; }
  const std::vector<std::string>& DeletedBlobIds() const { return deleted_blob_ids_; }
  uint32_t PresignCallCount() const { return presign_call_count_; }

private:
  std::vector<std::string> uploaded_bodies_;
  std::vector<std::string> retained_blob_ids_;
  std::vector<std::string> deleted_blob_ids_;
  std::optional<Error> presign_error_;
  std::optional<BlobListResult> list_result_;
  uint64_t next_blob_id_ = 1;
  uint32_t presign_call_count_ = 0;
};

class MockDirectoryClient : public IDirectoryClient {
public:
  void SetDefaultKemPublicKeyB64(std::string kem_public_key_b64) {
    default_kem_public_key_b64_ = std::move(kem_public_key_b64);
  }

  Roe<std::vector<DirectoryHit>> SearchPeople(const std::string& query) override;
  Roe<DirectoryHit> LookupRelayUser(const std::string& relay_user_id) override;
  Roe<DirectoryHit> LookupByAccount(const std::string& account_id) override;

private:
  std::string default_kem_public_key_b64_;
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
                                                 const std::string& signature_alg = "ml-dsa-65",
                                                 const std::string& kem_public_key_b64 = "",
                                                 const std::string& peer_id = "",
                                                 const std::vector<std::string>& multiaddrs = {},
                                                 const RegistrationPublishOpts& publish = {}) override;
  Roe<RegistrationResult> FinishRegistration(const std::string& challenge, const std::string& public_key_b64,
                                             const std::string& nickname, const std::string& signature,
                                             int64_t timestamp, const std::string& signature_alg = "ml-dsa-65",
                                             const std::string& kem_public_key_b64 = "",
                                             const std::string& peer_id = "",
                                             const std::vector<std::string>& multiaddrs = {},
                                             int64_t initiation_floor = 0,
                                             const RegistrationPublishOpts& publish = {}) override;
  Roe<RegistrationResult> UpdateNickname(const std::string& new_nickname, const std::string& signature,
                                         int64_t timestamp, const std::string& relay_user_id) override;
};

class HttpRelayClient : public IRelayClient {
public:
  explicit HttpRelayClient(std::string base_url);
  void SetAuthSigner(RelayAuthSigner signer) { auth_signer_ = std::move(signer); }
  Roe<void> Send(const RelayEnvelope& envelope) override;
  Roe<RelayPollResult> PollInbox(const std::string& requester_contact_id, const std::string& cursor) override;
  Roe<RelayDeleteResult> AckInbox(const std::string& requester_contact_id, const std::string& cursor) override;
  Roe<RelayDeleteResult> ClearInbox(const std::string& requester_contact_id,
                                    const std::string& before_created_at) override;
  Roe<ChatHistoryResponse> FetchChatHistory(const ChatHistoryRequest& request) override;

private:
  Roe<std::string> SignRelayApiBytes(const std::vector<uint8_t>& sign_bytes) const;

  std::string base_url_;
  RelayAuthSigner auth_signer_;
};

class HttpPushDeviceClient : public IPushDeviceClient {
public:
  explicit HttpPushDeviceClient(std::string base_url);
  void SetAuthSigner(RelayAuthSigner signer) { auth_signer_ = std::move(signer); }
  Roe<void> RegisterDevice(const PushDeviceRegistration& registration) override;
  Roe<void> UnregisterDevice(const PushDeviceRegistration& registration) override;

private:
  Roe<std::string> SignRelayApiBytes(const std::vector<uint8_t>& sign_bytes) const;
  Roe<void> PostDevice(const char* path, RelayApiOp op, const PushDeviceRegistration& registration);

  std::string base_url_;
  RelayAuthSigner auth_signer_;
};

class HttpDirectoryClient : public IDirectoryClient {
public:
  explicit HttpDirectoryClient(std::string base_url);
  Roe<std::vector<DirectoryHit>> SearchPeople(const std::string& query) override;
  Roe<DirectoryHit> LookupRelayUser(const std::string& relay_user_id) override;
  Roe<DirectoryHit> LookupByAccount(const std::string& account_id) override;
  Roe<std::vector<MeshNodeHit>> ListMeshNodes() override;

private:
  std::string base_url_;
};

/** Try each backend in order until one succeeds (N029 nd3). */
class FailoverDirectoryClient : public IDirectoryClient {
public:
  explicit FailoverDirectoryClient(std::vector<std::unique_ptr<IDirectoryClient>> backends);
  Roe<std::vector<DirectoryHit>> SearchPeople(const std::string& query) override;
  Roe<DirectoryHit> LookupRelayUser(const std::string& relay_user_id) override;
  Roe<DirectoryHit> LookupByAccount(const std::string& account_id) override;
  Roe<std::vector<MeshNodeHit>> ListMeshNodes() override;

private:
  std::vector<std::unique_ptr<IDirectoryClient>> backends_;
};

class HttpRegistrationClient : public IRegistrationClient {
public:
  explicit HttpRegistrationClient(std::string base_url);
  Roe<RegistrationStartResult> StartRegistration(const std::string& public_key_b64, const std::string& nickname,
                                                 const std::string& signature_alg = "ml-dsa-65",
                                                 const std::string& kem_public_key_b64 = "",
                                                 const std::string& peer_id = "",
                                                 const std::vector<std::string>& multiaddrs = {},
                                                 const RegistrationPublishOpts& publish = {}) override;
  Roe<RegistrationResult> FinishRegistration(const std::string& challenge, const std::string& public_key_b64,
                                             const std::string& nickname, const std::string& signature,
                                             int64_t timestamp, const std::string& signature_alg = "ml-dsa-65",
                                             const std::string& kem_public_key_b64 = "",
                                             const std::string& peer_id = "",
                                             const std::vector<std::string>& multiaddrs = {},
                                             int64_t initiation_floor = 0,
                                             const RegistrationPublishOpts& publish = {}) override;
  Roe<RegistrationResult> UpdateNickname(const std::string& new_nickname, const std::string& signature,
                                         int64_t timestamp, const std::string& relay_user_id) override;

private:
  std::string base_url_;
};

class MockClientCompatClient : public IClientCompatClient {
public:
  void SetDocument(ClientCompatDocument doc) {
    document_ = std::move(doc);
    has_document_ = true;
    error_.clear();
  }
  void SetError(std::string error) {
    error_ = std::move(error);
    has_document_ = false;
  }

  Roe<ClientCompatDocument> Fetch() override;

private:
  bool has_document_ = false;
  ClientCompatDocument document_;
  std::string error_;
};

class HttpClientCompatClient : public IClientCompatClient {
public:
  explicit HttpClientCompatClient(std::string base_url);
  Roe<ClientCompatDocument> Fetch() override;

private:
  std::string base_url_;
};

} // namespace pbr
