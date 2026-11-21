#pragma once

#include "common/Secret.h"
#include "lib/net/RelayAudience.h"

#include "mesh/types/DirectoryTypes.h"
#include "common/Error.h"
#include "common/ValueJson.h"
#include "mesh/types/IDirectoryClient.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

/** Optional register/finish extras (N027 mesh_node publish). */
struct RegistrationPublishOpts {
  /** Empty → omit (server defaults person). Values: person | mesh_node. */
  std::string entity_kind;
  bool has_capabilities = false;
  mesh::MeshCapabilitiesAd capabilities;
  /** What a mesh_node runs (N032): sent when set; the directory lists them as the node's own claims. */
  int amp_wire = 0;
  std::string node_version;
  std::vector<std::string> features;
  /**
   * The account's signed account card (ACCOUNT_CARD.md), on register/finish only: the relay stores the newest and
   * reads the account's relays and initiation floor from it (multi-relay MR002 / MR005). Unset: none sent (a pp-node).
   */
  std::optional<pp::common::Object> account_card;
};

/**
 * register/start's and register/finish's shared extras into `body`: entity_kind and capabilities (N027), what a
 * mesh_node runs (N032). Not the account card (finish only).
 */
void ApplyRegistrationPublishOpts(pp::common::Object& body, const RegistrationPublishOpts& publish);

/** A `GET /v1/mesh/nodes` body's rows (N027), with what each node says it runs (N032) when it says it. */
Roe<std::vector<mesh::MeshNodeHit>> MeshNodeHitsFromDirectoryJson(const std::string& json);

struct RegistrationResult {
  bool success = false;
  std::string account_id;
  std::string message;
  /**
   * The Brief LLM token register/finish answers (`llm_token`; only the org's relay's is kept, MR006), and when it
   * expires (`llm_token_expires_at`, ISO-8601, 7 days): renewed before then with `token/renew` (RenewLlmToken).
   */
  SecretText llm_token;
  std::string llm_token_expires_at;
  /** ISO-8601 registration expiry from register/finish. */
  std::string expires_at;
  /**
   * The registration's id at that relay (`registration_id`, MR007 item 5): new for a fresh registration, the same
   * across its renewals; a revoke names it. Empty when the relay gave none.
   */
  std::string registration_id;
  /**
   * What the relay did with the account card sent (`stored`, `same`, `stale`); empty when none was sent. `stale`
   * (X12): the relay holds a newer card (another device of the account wrote it); the registration still holds. Read
   * through AccountCardStale.
   */
  std::string account_card_status;
  /** Set by the registration flow, not the wire: the `issued_at` of the card the relay answered for (0: none). */
  int64_t account_card_issued_at = 0;
  /** The directory entry now carries the account's KEM key binding (the relay stored it). */
  bool kem_bound = false;
  /** Set by the registration flow, not the wire: the normalized base URL of the registry that finished it. */
  std::string registered_backend_url;
};

/** `account_card_status` `stale`: the relay keeps a newer card than the one sent (X12). */
inline bool AccountCardStale(const RegistrationResult& result) {
  return result.account_card_status == "stale";
}

/** What `POST /api/llm/v1/token/renew` answers: a fresh Brief LLM token and when it expires (ISO-8601). */
struct LlmTokenRenewal {
  SecretText llm_token;
  std::string llm_token_expires_at;
};

struct RegistrationStartResult {
  std::string challenge;
  std::string signature_alg;
  std::string expires_at;
};

/** The relay's registry: register (start, finish), the nickname, revoke. The signed ones name the relay (MR007). */
class IRegistrationClient {
public:
  virtual ~IRegistrationClient() = default;
  /** The registry this client talks to; empty when unknown (the registration is then not bound to a backend). */
  virtual std::string BaseUrl() const { return {}; }
  /** Who this relay's commands are signed for (its pin); none: nothing can be signed for it. */
  virtual IRelayAudience* Audience() const = 0;
  virtual Roe<RegistrationStartResult> StartRegistration(const std::string& public_key_b64, const std::string& nickname,
                                                         const std::string& signature_alg = "ml-dsa-65",
                                                         const std::string& kem_public_key_b64 = "",
                                                         const std::string& peer_id = "",
                                                         const std::vector<std::string>& multiaddrs = {},
                                                         const RegistrationPublishOpts& publish = {}) = 0;
  /** `signed_finish`: the audience, timestamp and signature over BuildRegistrationSignBytes. */
  virtual Roe<RegistrationResult> FinishRegistration(
      const std::string& challenge, const std::string& public_key_b64, const std::string& nickname,
      const SignedRelayCommand& signed_finish, const std::string& signature_alg = "ml-dsa-65",
      const std::string& kem_public_key_b64 = "", const std::string& kem_binding_signature_b64 = "",
      const std::string& peer_id = "", const std::vector<std::string>& multiaddrs = {},
      const RegistrationPublishOpts& publish = {}) = 0;
  virtual Roe<RegistrationResult> UpdateNickname(const std::string& new_nickname,
                                                 const SignedRelayCommand& signed_update,
                                                 const std::string& account_id) = 0;
  /**
   * Removes this account's registration `registration_id` (#328, MR007; BuildAccountRevokeSignBytes). Another id held
   * there now: Err::Auth::ScopeMismatch (`409 registration_mismatch`), nothing removed.
   */
  virtual Roe<void> RevokeRegistration(const std::string& account_id, const std::string& registration_id,
                                       const SignedRelayCommand& signed_revoke) = 0;
  /**
   * Whether this relay's Brief LLM token is the account's (the org's relay, MR006): its register/finish token is
   * kept, and RenewLlmToken renews it.
   */
  virtual bool TakesBriefLlmKey() const { return false; }
  /**
   * A fresh Brief LLM token from this relay's `POST /api/llm/v1/token/renew` (signed: BuildLlmTokenRenewSignBytes,
   * RC018 item 2); it does not invalidate the token held now. Not every client can (default: an error).
   */
  virtual Roe<LlmTokenRenewal> RenewLlmToken(const std::string& account_id, const SignedRelayCommand& signed_renew) {
    (void)account_id;
    (void)signed_renew;
    return Error("This registry does not renew Brief LLM tokens");
  }
};

} // namespace pbr
