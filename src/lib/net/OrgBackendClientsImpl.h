#pragma once

#include "mesh/types/DirectoryTypes.h"
#include "lib/net/BlobClient.h"
#include "lib/net/ClientCompat.h"
#include "lib/net/IPushDeviceClient.h"
#include "lib/net/RelayApiSignPayload.h"
#include "lib/net/OrgBackendClients.h"

#include <string>
#include <vector>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

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

class HttpDirectoryClient : public mesh::IDirectoryClient {
public:
  explicit HttpDirectoryClient(std::string base_url);
  Roe<std::vector<mesh::DirectoryHit>> SearchPeople(const std::string& query) override;
  Roe<mesh::DirectoryHit> LookupRelayUser(const std::string& relay_user_id) override;
  Roe<mesh::DirectoryHit> LookupByAccount(const std::string& account_id) override;
  Roe<std::vector<mesh::MeshNodeHit>> ListMeshNodes() override;

private:
  std::string base_url_;
};

/** Try each backend in order until one succeeds (N029 nd3). */
class FailoverDirectoryClient : public mesh::IDirectoryClient {
public:
  explicit FailoverDirectoryClient(std::vector<std::unique_ptr<mesh::IDirectoryClient>> backends);
  Roe<std::vector<mesh::DirectoryHit>> SearchPeople(const std::string& query) override;
  Roe<mesh::DirectoryHit> LookupRelayUser(const std::string& relay_user_id) override;
  Roe<mesh::DirectoryHit> LookupByAccount(const std::string& account_id) override;
  Roe<std::vector<mesh::MeshNodeHit>> ListMeshNodes() override;

private:
  std::vector<std::unique_ptr<mesh::IDirectoryClient>> backends_;
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
                                             const std::string& kem_binding_signature_b64 = "",
                                             const std::string& peer_id = "",
                                             const std::vector<std::string>& multiaddrs = {},
                                             int64_t initiation_floor = 0,
                                             const RegistrationPublishOpts& publish = {}) override;
  Roe<RegistrationResult> UpdateNickname(const std::string& new_nickname, const std::string& signature,
                                         int64_t timestamp, const std::string& relay_user_id) override;
  Roe<void> RevokeRegistration(const std::string& relay_user_id, const std::string& signature,
                               int64_t timestamp) override;

private:
  std::string base_url_;
};

class HttpClientCompatClient : public IClientCompatClient {
public:
  explicit HttpClientCompatClient(std::string base_url);
  Roe<ClientCompatDocument> Fetch() override;

private:
  std::string base_url_;
};

} // namespace pbr
