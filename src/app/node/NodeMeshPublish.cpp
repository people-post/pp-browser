#include "app/node/NodeMeshPublish.h"

#include "domain/net/HttpClient.h"
#include "feature/conversations/RegistrationClientUtil.h"
#include "domain/net/RegistrationSignPayload.h"
#include "common/Utilities.h"
#include "common/ValueJson.h"

#include <iostream>
#include <utility>

namespace pbr {
namespace {

/** Headless-safe registration client (no pp_domain_net / messaging). */
class ThinHttpRegistrationClient : public IRegistrationClient {
public:
  explicit ThinHttpRegistrationClient(std::string base_url) : base_url_(std::move(base_url)) {}

  Roe<RegistrationStartResult> StartRegistration(const std::string& public_key_b64, const std::string& nickname,
                                                 const std::string& signature_alg,
                                                 const std::string& kem_public_key_b64, const std::string& peer_id,
                                                 const std::vector<std::string>& multiaddrs,
                                                 const RegistrationPublishOpts& publish) override {
    if (base_url_.empty()) {
      return Error("Registration base_url not configured");
    }
    if (kem_public_key_b64.empty()) {
      return Error("kem_public_key_b64 is required");
    }
    Object body;
    body.set("public_key", public_key_b64);
    body.set("nickname", nickname);
    body.set("signature_alg", signature_alg);
    body.set("kem_public_key_b64", kem_public_key_b64);
    if (!peer_id.empty()) {
      body.set("peer_id", peer_id);
    }
    if (!multiaddrs.empty()) {
      std::vector<Value> addrs;
      for (const std::string& ma : multiaddrs) {
        addrs.push_back(Value(ma));
      }
      body.set("multiaddrs", ArrayValue(std::move(addrs)));
    }
    ApplyPublish(body, publish);
    const auto response = HttpClient::Post(base_url_ + "/v1/register/start", DumpJson(body),
                                           {{"Content-Type", "application/json"}});
    if (!response) {
      return response.error();
    }
    if (response.value().status_code < 200 || response.value().status_code >= 300) {
      return Error("Registration start failed with status " + std::to_string(response.value().status_code));
    }
    auto root = TryParseObject(response.value().body);
    auto challenge = root ? root->getString("challenge") : std::nullopt;
    if (!challenge) {
      return Error("Invalid registration start JSON");
    }
    RegistrationStartResult result;
    result.challenge = *challenge;
    result.signature_alg = root->getString("signature_alg").value_or(signature_alg);
    if (auto expires = root->getString("expires_at")) {
      result.expires_at = *expires;
    }
    return result;
  }

  Roe<RegistrationResult> FinishRegistration(const std::string& challenge, const std::string& public_key_b64,
                                             const std::string& nickname, const std::string& signature,
                                             int64_t timestamp, const std::string& signature_alg,
                                             const std::string& kem_public_key_b64, const std::string& peer_id,
                                             const std::vector<std::string>& multiaddrs, int64_t initiation_floor,
                                             const RegistrationPublishOpts& publish) override {
    if (base_url_.empty()) {
      return Error("Registration base_url not configured");
    }
    if (kem_public_key_b64.empty()) {
      return Error("kem_public_key_b64 is required");
    }
    Object body;
    body.set("challenge", challenge);
    body.set("public_key", public_key_b64);
    body.set("nickname", nickname);
    body.set("signature", signature);
    body.set("timestamp", timestamp);
    body.set("signature_alg", signature_alg);
    body.set("kem_public_key_b64", kem_public_key_b64);
    body.set("initiation_floor", initiation_floor);
    if (!peer_id.empty()) {
      body.set("peer_id", peer_id);
    }
    if (!multiaddrs.empty()) {
      std::vector<Value> addrs;
      for (const std::string& ma : multiaddrs) {
        addrs.push_back(Value(ma));
      }
      body.set("multiaddrs", ArrayValue(std::move(addrs)));
    }
    ApplyPublish(body, publish);
    const auto response = HttpClient::Post(base_url_ + "/v1/register/finish", DumpJson(body),
                                           {{"Content-Type", "application/json"}});
    if (!response) {
      return response.error();
    }
    if (response.value().status_code < 200 || response.value().status_code >= 300) {
      return Error("Registration finish failed with status " + std::to_string(response.value().status_code));
    }
    auto root = TryParseObject(response.value().body);
    RegistrationResult result{.success = true};
    if (root) {
      if (auto success = root->getIf<bool>("success")) {
        result.success = *success;
      }
      if (auto relay_user_id = root->getString("relay_user_id")) {
        result.relay_user_id = *relay_user_id;
      }
      if (auto message = root->getString("message")) {
        result.message = *message;
      }
      if (auto llm_api_key = root->getString("llm_api_key")) {
        result.llm_api_key = *llm_api_key;
      }
      if (auto expires = root->getString("expires_at")) {
        result.expires_at = *expires;
      }
    }
    return result;
  }

  Roe<RegistrationResult> UpdateNickname(const std::string& /*new_nickname*/, const std::string& /*signature*/,
                                         int64_t /*timestamp*/, const std::string& /*relay_user_id*/) override {
    return Error("UpdateNickname not supported on pp-node thin client");
  }

private:
  static void ApplyPublish(Object& body, const RegistrationPublishOpts& publish) {
    if (!publish.entity_kind.empty()) {
      body.set("entity_kind", publish.entity_kind);
    }
    if (publish.has_capabilities) {
      Object caps;
      caps.set("circuit_relay", publish.capabilities.circuit_relay);
      caps.set("media_relay", publish.capabilities.media_relay);
      caps.set("dht", publish.capabilities.dht);
      caps.set("ledger_gateway", publish.capabilities.ledger_gateway);
      body.set("capabilities", std::move(caps));
    }
  }

  std::string base_url_;
};

} // namespace

Roe<bool> PublishOrRenewMeshNodeListing(const AppConfig& config, IdentityStore& identity,
                                        const std::string& nickname) {
  if (!config.mesh.mesh_publish) {
    return false;
  }
  if (config.registration.base_url.empty()) {
    return Error("mesh_publish requires registration.base_url");
  }
  if (config.mesh.advertise_multiaddrs.empty()) {
    return Error("mesh_publish requires mesh.advertise_multiaddrs (public multiaddrs)");
  }

  ThinHttpRegistrationClient registration(config.registration.base_url);
  RegistrationPublishOpts publish;
  publish.entity_kind = "mesh_node";
  publish.has_capabilities = true;
  publish.capabilities.circuit_relay = config.mesh.capabilities.circuit_relay;
  publish.capabilities.media_relay = config.mesh.capabilities.media_relay;
  publish.capabilities.dht = config.mesh.capabilities.dht;
  publish.capabilities.ledger_gateway = config.mesh.capabilities.ledger_gateway;

  std::string nick = nickname;
  if (nick.empty()) {
    auto loaded = identity.Get();
    if (loaded && !loaded->nickname.empty()) {
      nick = loaded->nickname;
    } else {
      nick = "pp-node";
    }
  }

  auto persisted =
      FinishAndPersistRegistration(registration, identity, nick, config.mesh.advertise_multiaddrs, publish);
  if (!persisted) {
    return persisted.error();
  }
  std::cerr << "pp-node: mesh directory publish ok account=" << persisted->account_id
            << " peer=" << persisted->peer_id << " expires=" << persisted->registration_expires_at << "\n";
  return true;
}

} // namespace pbr
