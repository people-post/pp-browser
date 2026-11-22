#pragma once

#include "mesh/types/DirectoryTypes.h"
#include "lib/net/BlobClient.h"
#include "lib/net/ClientCompat.h"
#include "lib/net/HttpClient.h"
#include "lib/net/IPushDeviceClient.h"
#include "lib/net/RelayApiSignPayload.h"
#include "lib/net/OrgBackendClients.h"

#include <functional>
#include <atomic>
#include <memory>
#include <mutex>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

/**
 * This device's push binding on one relay. It keeps, per device id, the `binding_id` its last register answered (in memory: the app
 * registers at every start); an unregister names it, and one with none kept, or answered `binding_mismatch` (a newer
 * binding there), registers first and unregisters with that answer, once (MR007 item 5).
 */
class HttpPushDeviceClient : public IPushDeviceClient {
public:
  /** `public_only`: a relay the person added (not configured): https to a public address only, as a contact's. */
  explicit HttpPushDeviceClient(std::string base_url, bool public_only = false);
  void SetAuthSigner(RelayAuthSigner signer) { auth_signer_ = std::move(signer); }
  void SetAudience(std::shared_ptr<IRelayAudience> audience) { audience_ = std::move(audience); }
  Roe<void> RegisterDevice(const PushDeviceRegistration& registration) override;
  Roe<void> UnregisterDevice(const PushDeviceRegistration& registration) override;

private:
  /** One signed POST to `path` (its last field `last_key`); the answer's body on success. */
  Roe<std::string> PostDevice(const char* path, const PushDeviceRegistration& registration, const char* last_key,
                              const std::string& last, int64_t timestamp, const RelayCommandBytes& build);
  Roe<void> UnregisterOnce(const PushDeviceRegistration& registration, const std::string& binding_id);

  std::string base_url_;
  bool public_only_ = false;
  RelayAuthSigner auth_signer_;
  std::shared_ptr<IRelayAudience> audience_;
  std::mutex binding_mutex_;
  /** The newest binding_id this relay answered for each device id (the `.voip` sibling has its own). */
  std::map<std::string, std::string> binding_ids_;
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
  /**
   * `timeout` bounds each request; `cards`, when given, gets the card a lookup answer carries; `public_only`: https to
   * a public address only (a relay the person added).
   */
  explicit HttpDirectoryClient(std::string base_url, HttpTimeout timeout = {},
                               std::shared_ptr<const DirectoryCardSink> cards = nullptr, bool public_only = false);
  Roe<std::vector<mesh::DirectoryHit>> SearchPeople(const std::string& query) override;
  Roe<mesh::DirectoryHit> LookupByAccount(const std::string& account_id) override;
  Roe<std::vector<mesh::MeshNodeHit>> ListMeshNodes() override;

private:
  std::string base_url_;
  HttpTimeout timeout_;
  std::shared_ptr<const DirectoryCardSink> cards_;
  bool public_only_ = false;
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
 * card only once they verify (AccountKeyResolver). `public_only`: a relay not ours (a contact's card or QR code named
 * it): an origin outside IsPublicRelayOrigin is not asked at all, and the request goes over https to a public address
 * only (no proxy, no redirect), as a contact's relay client's do.
 */
Roe<std::optional<StoredAccountRow>> LookupStoredAccount(const std::string& relay_base_url,
                                                         const std::string& account_id, bool public_only = false);

/**
 * The account card `account_id` registered with on the relay at `relay_base_url` (LookupStoredAccount's `card`);
 * none when the account is not registered there or gave none. `public_only` as there.
 */
Roe<std::optional<pp::common::Object>> FetchStoredAccountCard(const std::string& relay_base_url,
                                                              const std::string& account_id, bool public_only = false);

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
  /** `public_only` as HttpPushDeviceClient's. */
  explicit HttpRegistrationClient(std::string base_url, bool public_only = false);
  std::string BaseUrl() const override { return base_url_; }
  /** Who register/finish, the profile and the revoke are signed for (the relay's pin, MR007). */
  void SetAudience(std::shared_ptr<IRelayAudience> audience) { audience_ = std::move(audience); }
  IRelayAudience* Audience() const override { return audience_.get(); }
  /**
   * Whether this relay's `llm_token` is kept (default yes). The Brief LLM token is the org's (multi-relay MR006):
   * a relay other than the org's answers with a token its own gateway takes, not the org's, so it is left out.
   */
  void SetTakesBriefLlmKey(bool takes) { takes_llm_key_.store(takes, std::memory_order_relaxed); }
  bool TakesBriefLlmKey() const override { return takes_llm_key_.load(std::memory_order_relaxed); }
  Roe<RegistrationStartResult> StartRegistration(const std::string& public_key_b64, const std::string& nickname,
                                                 const std::string& kem_public_key_b64 = "",
                                                 const std::string& peer_id = "",
                                                 const std::vector<std::string>& multiaddrs = {},
                                                 const RegistrationPublishOpts& publish = {}) override;
  Roe<RegistrationResult> FinishRegistration(const std::string& challenge, const std::string& public_key_b64,
                                             const std::string& nickname, const SignedRelayCommand& signed_finish,
                                             const std::string& kem_public_key_b64 = "",
                                             const std::string& kem_binding_b64 = "", const std::string& peer_id = "",
                                             const std::vector<std::string>& multiaddrs = {},
                                             const RegistrationPublishOpts& publish = {}) override;
  Roe<RegistrationResult> UpdateNickname(const std::string& new_nickname, const SignedRelayCommand& signed_update,
                                         const std::string& account_id) override;
  Roe<void> RevokeRegistration(const std::string& account_id, const std::string& registration_id,
                               const SignedRelayCommand& signed_revoke) override;
  /** `POST <origin>/api/llm/v1/token/renew` on this relay's origin (the registry serves it there, SPECS). */
  Roe<LlmTokenRenewal> RenewLlmToken(const std::string& account_id, const SignedRelayCommand& signed_renew) override;

private:
  std::string base_url_;
  bool public_only_ = false;
  std::shared_ptr<IRelayAudience> audience_;
  /** Set on the owner's thread, read by a registration on a worker. */
  std::atomic<bool> takes_llm_key_{true};
};

class HttpClientCompatClient : public IClientCompatClient {
public:
  /** `public_only`: https to a public address only (Add's probe of a relay the person typed). */
  explicit HttpClientCompatClient(std::string base_url, bool public_only = false);
  Roe<ClientCompatDocument> Fetch() override;

private:
  std::string base_url_;
  bool public_only_ = false;
};

} // namespace pbr
