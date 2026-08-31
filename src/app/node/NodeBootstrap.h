#pragma once

#include "base/crypto/ProfileSecretsService.h"
#include "base/data/Config.h"
#include "base/people/IdentityStore.h"
#include "base/runtime/AppRuntime.h"
#include "common/Error.h"
#include "base/p2p/MeshHost.h"

#include <memory>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

struct NodeBootstrapOptions {
  int argc = 0;
  char** argv = nullptr;
  std::string pin;
  std::string profile_override;
  std::string listen_override;
  /** When true, use desktop listen fallback (N016 opt-in). Default fail-loud. */
  bool listen_fallback = false;
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
