#pragma once

#include "base/data/Config.h"
#include "base/people/IdentityStore.h"
#include "common/Error.h"
#include "libp2p/integration/host/DialBackService.h"
#include "libp2p/integration/host/CircuitRelayService.h"
#include "libp2p/integration/host/MediaRelayService.h"
#include "libp2p/integration/host/NodeRuntime.h"
#include "libp2p/integration/host/ReachabilityService.h"

#include <memory>
#include <string>

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
  std::unique_ptr<IdentityStore> identity;
  std::unique_ptr<NodeRuntime> runtime;
  std::unique_ptr<DialBackService> dial_back;
  std::unique_ptr<CircuitRelayService> circuit_relay;
  std::unique_ptr<MediaRelayService> media_relay;
  /** Heap-allocated: ReachabilityService is not movable (mutex). */
  std::unique_ptr<ReachabilityService> reachability;
};

/** Headless node bootstrap: config + PIN unlock + NodeRuntime + dial-back (N011). */
Roe<NodeBootstrapResult> BootstrapPpNode(const NodeBootstrapOptions& options);

} // namespace pbr
