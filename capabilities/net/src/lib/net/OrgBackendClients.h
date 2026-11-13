#pragma once

#include "common/Secret.h"

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

using RelayAuthSigner = std::function<Roe<std::string>(const std::vector<uint8_t>&)>;

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
};

/** register/finish's extras into `body`: entity_kind and capabilities (N027), what a mesh_node runs (N032). */
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
  /** Echoed initiation floor when server supports it; missing → leave unset (0). */
  int64_t initiation_floor = 0;
  bool initiation_floor_present = false;
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

class IRegistrationClient {
public:
  virtual ~IRegistrationClient() = default;
  /** The registry this client talks to; empty when unknown (the registration is then not bound to a backend). */
  virtual std::string BaseUrl() const { return {}; }
  virtual Roe<RegistrationStartResult> StartRegistration(const std::string& public_key_b64, const std::string& nickname,
                                                         const std::string& signature_alg = "ml-dsa-65",
                                                         const std::string& kem_public_key_b64 = "",
                                                         const std::string& peer_id = "",
                                                         const std::vector<std::string>& multiaddrs = {},
                                                         const RegistrationPublishOpts& publish = {}) = 0;
  virtual Roe<RegistrationResult> FinishRegistration(
      const std::string& challenge, const std::string& public_key_b64, const std::string& nickname,
      const std::string& signature, int64_t timestamp, const std::string& signature_alg = "ml-dsa-65",
      const std::string& kem_public_key_b64 = "", const std::string& kem_binding_signature_b64 = "",
      const std::string& peer_id = "", const std::vector<std::string>& multiaddrs = {}, int64_t initiation_floor = 0,
      const RegistrationPublishOpts& publish = {}) = 0;
  virtual Roe<RegistrationResult> UpdateNickname(const std::string& new_nickname, const std::string& signature,
                                                 int64_t timestamp, const std::string& account_id) = 0;
  /** Remove this account's directory registration (#328; `BuildAccountRevokeSignBytes` + signature). */
  virtual Roe<void> RevokeRegistration(const std::string& account_id, const std::string& signature,
                                       int64_t timestamp) = 0;
};

} // namespace pbr
