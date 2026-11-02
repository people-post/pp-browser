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

  bool operator==(const ServiceEndpointConfig& other) const {
    return base_url == other.base_url && transport == other.transport;
  }
  bool operator!=(const ServiceEndpointConfig& other) const { return !(*this == other); }
};

/**
 * Directory / phone-book endpoints (N029 nd3).
 * Legacy: `base_url` (+ optional `transport`).
 * Preferred: ordered `providers[]` with failover; empty providers → synthesize from base_url.
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
