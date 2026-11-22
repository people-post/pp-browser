#pragma once

#include <string>
#include <vector>

namespace pbr {

// The backend's endpoints from the app's config, as values (projects/capability-repos CR015): the app reads them
// from its config file (foundation/data) and passes them to what builds the clients (lib/net).

struct ServiceEndpointConfig {
  std::string base_url;
  /** http now; amp = MeshHost AmpDirectoryProtocol (N029 nd4; not CreateOrgBackendClients). */
  std::string transport = "http";
  /**
   * The relay's id, `relay:…` (multi-relay MR008 as release-compatibility SIGNED_FORMATS § 3.3 amends it: a relay
   * address carries it): what its certificate must prove (`GET /v1/relay-identity`); the audience of its signed calls
   * is that certificate's transport PeerId. `relay` / `registration` only; the build's for prod / staging / dev (`pending` while
   * that environment's operator set is not made: the relay is not used), the config's `relay_id` for a custom
   * backend. Empty: none named (where first use is allowed, the app pins its first valid certificate, with a warning).
   */
  std::string relay_id;

  bool operator==(const ServiceEndpointConfig& other) const {
    return base_url == other.base_url && transport == other.transport && relay_id == other.relay_id;
  }
  bool operator!=(const ServiceEndpointConfig& other) const { return !(*this == other); }
};

/**
 * Directory / phone-book endpoints (N029 nd3): one provider, `base_url` (+ `transport`), as the defaults and the
 * backend choice set it; or an ordered `providers[]` with failover, whose first base_url mirrors.
 */
struct DirectoryConfig {
  std::string base_url;
  std::string transport = "http";
  std::vector<ServiceEndpointConfig> providers;

  bool operator==(const DirectoryConfig& other) const {
    return base_url == other.base_url && transport == other.transport && providers == other.providers;
  }
  bool operator!=(const DirectoryConfig& other) const { return !(*this == other); }
};

/** Effective ordered provider list (providers, else single base_url entry). */
std::vector<ServiceEndpointConfig> EffectiveDirectoryProviders(const DirectoryConfig& config);

/** If base_url empty but providers set, mirror providers[0] into base_url/transport. */
void NormalizeDirectoryConfig(DirectoryConfig& config);

} // namespace pbr
