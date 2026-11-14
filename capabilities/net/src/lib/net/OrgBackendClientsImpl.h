#pragma once

#include "mesh/types/DirectoryTypes.h"
#include "lib/net/BlobClient.h"
#include "lib/net/ClientCompat.h"
#include "lib/net/HttpClient.h"
#include "lib/net/IPushDeviceClient.h"
#include "lib/net/RelayApiSignPayload.h"
#include "lib/net/OrgBackendClients.h"

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

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

/**
 * Where the account cards that directory lookups carry go (`GET /v1/users/by-account`'s `account_card`, multi-relay
 * R2): an account's relays and its price are its card's, so the owner keeps the card once it verifies. Unchecked here.
 * Set and cleared by the owner; a card given while it is cleared is dropped. Thread-safe.
 */
class DirectoryCardSink {
public:
  using Keep = std::function<void(const std::string& account_id, const Object& card)>;

  void Set(Keep keep);
  void Give(const std::string& account_id, const Object& card) const;

private:
  mutable std::mutex mutex_;
  Keep keep_;
};

class HttpDirectoryClient : public mesh::IDirectoryClient {
public:
  /** `timeout` bounds each request; `cards`, when given, gets the card a lookup answer carries. */
  explicit HttpDirectoryClient(std::string base_url, HttpTimeout timeout = {},
                               std::shared_ptr<const DirectoryCardSink> cards = nullptr);
  Roe<std::vector<mesh::DirectoryHit>> SearchPeople(const std::string& query) override;
  Roe<mesh::DirectoryHit> LookupByAccount(const std::string& account_id) override;
  Roe<std::vector<mesh::MeshNodeHit>> ListMeshNodes() override;

private:
  std::string base_url_;
  HttpTimeout timeout_;
  std::shared_ptr<const DirectoryCardSink> cards_;
};

/** What a relay keeps for an account (`GET /v1/users/by-account/{account_id}`): its row, and the card it registered. */
struct StoredAccountRow {
  /** The row: keys, nickname, photo, device endpoints (as a directory hit). */
  mesh::DirectoryHit hit;
  /** The account card it registered with (`account_card`, multi-relay R2 step 2); none when it gave none. */
  std::optional<pp::common::Object> card;
};

/**
 * The row the relay at `relay_base_url` keeps for `account_id`, unauthenticated (the lookup is public); none when the
 * account is not registered there (404). A row for another account is an error. Unchecked: the caller keeps keys and
 * card only once they verify (AccountKeyResolver).
 */
Roe<std::optional<StoredAccountRow>> LookupStoredAccount(const std::string& relay_base_url,
                                                         const std::string& account_id);

/**
 * The account card `account_id` registered with on the relay at `relay_base_url` (LookupStoredAccount's `card`);
 * none when the account is not registered there or gave none.
 */
Roe<std::optional<pp::common::Object>> FetchStoredAccountCard(const std::string& relay_base_url,
                                                              const std::string& account_id);

/** Try each backend in order until one succeeds (N029 nd3). */
class FailoverDirectoryClient : public mesh::IDirectoryClient {
public:
  explicit FailoverDirectoryClient(std::vector<std::unique_ptr<mesh::IDirectoryClient>> backends);
  Roe<std::vector<mesh::DirectoryHit>> SearchPeople(const std::string& query) override;
  Roe<mesh::DirectoryHit> LookupByAccount(const std::string& account_id) override;
  Roe<std::vector<mesh::MeshNodeHit>> ListMeshNodes() override;

private:
  std::vector<std::unique_ptr<mesh::IDirectoryClient>> backends_;
};

/**
 * The error for a register/start or register/finish answered `status_code` (not 2xx): a 4xx but 408 / 429 is the
 * server refusing the registration (Network HttpRejected: sending it again will not help), anything else HttpError.
 * `step` is "start" or "finish"; the message keeps its old words.
 */
Error RegistrationHttpStatusError(const std::string& step, long status_code);

class HttpRegistrationClient : public IRegistrationClient {
public:
  explicit HttpRegistrationClient(std::string base_url);
  std::string BaseUrl() const override { return base_url_; }
  /**
   * Whether this relay's `llm_api_key` is kept (default yes). The Brief LLM key is the org's (multi-relay MR006):
   * a relay other than the org's answers with a key its own gateway takes, not the org's, so it is left out.
   */
  void SetTakesBriefLlmKey(bool takes) { takes_llm_key_ = takes; }
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
                                             const RegistrationPublishOpts& publish = {}) override;
  Roe<RegistrationResult> UpdateNickname(const std::string& new_nickname, const std::string& signature,
                                         int64_t timestamp, const std::string& account_id) override;
  Roe<void> RevokeRegistration(const std::string& account_id, const std::string& signature, int64_t timestamp) override;

private:
  std::string base_url_;
  bool takes_llm_key_ = true;
};

class HttpClientCompatClient : public IClientCompatClient {
public:
  explicit HttpClientCompatClient(std::string base_url);
  Roe<ClientCompatDocument> Fetch() override;

private:
  std::string base_url_;
};

} // namespace pbr
