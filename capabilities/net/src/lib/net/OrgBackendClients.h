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
  /** Brief LLM API key returned once from register/finish or rotate. */
  SecretText llm_api_key;
  /** ISO-8601 registration expiry from register/finish. */
  std::string expires_at;
  /**
   * The registration's id at that relay (`registration_id`, MR007 item 5): new for a fresh registration, the same
   * across its renewals; a revoke names it. Empty when the relay gave none.
   */
  std::string registration_id;
  /** What the relay did with the account card sent (`stored`, `same`, `stale`); empty when none was sent. */
  std::string account_card_status;
  /** Set by the registration flow, not the wire: the `issued_at` of the card the relay answered for (0: none). */
  int64_t account_card_issued_at = 0;
  /** The directory entry now carries the account's KEM key binding (the relay stored it). */
  bool kem_bound = false;
  /** Set by the registration flow, not the wire: the normalized base URL of the registry that finished it. */
  std::string registered_backend_url;
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
};

} // namespace pbr
