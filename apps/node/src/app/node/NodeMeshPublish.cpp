#include "app/node/NodeMeshPublish.h"
#include "app/node/NodeEnvOverlay.h"
#include "foundation/product/DevTools.h"

#include "amp/L1/Types.h"
#include "foundation/product/AppVersion.h"

#include "domain/registration/RegistrationClient.h"
#include "lib/net/OrgBackendClientsImpl.h"

#include <iostream>
#include <utility>
#include "common/PbrCompat.h"

namespace pbr {

Roe<bool> PublishOrRenewMeshNodeListing(const NodeConfig& config, IdentityStore& identity, const std::string& nickname,
                                        std::shared_ptr<IRelayAudience> audience) {
  if (!config.mesh.mesh_publish) {
    return false;
  }
  if (config.registration.base_url.empty()) {
    return Error("mesh_publish requires registration.base_url");
  }
  if (config.mesh.advertise_multiaddrs.empty()) {
    return Error("mesh_publish requires mesh.advertise_multiaddrs (public multiaddrs)");
  }
  if (const std::string problem = NodeRegistrationIdProblem(config.registration, kDeveloperRelaysAllowed);
      !problem.empty()) {
    return Error(problem); // MR008 item 5: refused before anything is asked or signed
  }

  HttpRegistrationClient registration(config.registration.base_url);
  registration.SetAudience(std::move(audience));
  RegistrationPublishOpts publish;
  publish.entity_kind = "mesh_node";
  publish.has_capabilities = true;
  publish.capabilities.circuit_relay = config.mesh.capabilities.circuit_relay;
  publish.capabilities.media_relay = config.mesh.capabilities.media_relay;
  publish.capabilities.dht = config.mesh.capabilities.dht;
  publish.capabilities.ledger_gateway = config.mesh.capabilities.ledger_gateway;
  publish.amp_wire = pp::adp::kWireVersion;
  publish.node_version = AppVersionString();

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
  std::cerr << "pp-node: mesh directory publish ok account=" << persisted->account_id << " peer=" << persisted->peer_id
            << " expires=" << persisted->registration_expires_at << "\n";
  return true;
}

} // namespace pbr
