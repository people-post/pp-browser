#pragma once

#include "common/Secret.h"

#include "foundation/crypto/ProfileSecretsEngine.h"
#include "foundation/data/Config.h"
#include "lib/people/IdentityStore.h"
#include "common/runtime/AppRuntime.h"
#include "common/Error.h"
#include "mesh/host/MeshHost.h"

#include <memory>
#include <string>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

struct NodeBootstrapOptions {
  int argc = 0;
  char** argv = nullptr;
  SecretText pin;
  std::string profile_override;
};

struct NodeBootstrapResult {
  AppConfig config;
  std::string data_dir;
  std::string profile_data_dir;
  std::string profile_id;
  /** The config file loaded (`--config` / PP_BROWSER_CONFIG); empty: defaults and env overlays only. */
  std::string config_path;
  /** Node-owned profile vault / DEK service (holds identity DEK consumer). */
  std::unique_ptr<ProfileSecretsEngine> secrets;
  std::unique_ptr<IdentityStore> identity;
  /** Shared Amp mesh host: AmpStack + L4 coordinators + reachability. */
  std::unique_ptr<mesh::MeshHost> mesh;
};

/** Headless node bootstrap: config + PIN unlock + Amp mesh + dial-back (N011). */
Roe<NodeBootstrapResult> BootstrapPpNode(const NodeBootstrapOptions& options);

} // namespace pbr
