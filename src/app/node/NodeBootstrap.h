#pragma once

#include "foundation/crypto/ProfileSecretsService.h"
#include "foundation/data/Config.h"
#include "domain/people/IdentityStore.h"
#include "foundation/runtime/AppRuntime.h"
#include "common/Error.h"
#include "domain/mesh/host/MeshHost.h"

#include <memory>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

struct NodeBootstrapOptions {
  int argc = 0;
  char** argv = nullptr;
  std::string pin;
  std::string profile_override;
};

struct NodeBootstrapResult {
  AppConfig config;
  std::string data_dir;
  std::string profile_data_dir;
  std::string profile_id;
  std::string config_path;
  /** Node-owned profile vault / DEK service (holds identity DEK consumer). */
  std::unique_ptr<ProfileSecretsService> secrets;
  std::unique_ptr<IdentityStore> identity;
  /** Shared Amp mesh host: AmpStack + L4 coordinators + reachability. */
  std::unique_ptr<MeshHost> mesh;
};

/** Headless node bootstrap: config + PIN unlock + Amp mesh + dial-back (N011). */
Roe<NodeBootstrapResult> BootstrapPpNode(const NodeBootstrapOptions& options);

} // namespace pbr
