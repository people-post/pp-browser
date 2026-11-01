#pragma once

#include "lib/delivery/HttpRelayClient.h"
#include "lib/net/HttpBlobClient.h"
#include "lib/net/HttpLockerClient.h"
#include "lib/net/OrgBackendClientsImpl.h"
#include "lib/net/SwitchableDirectoryClient.h"
#include "common/Module.h"
#include "common/ServiceEndpoints.h"

#include <memory>
#include <string>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

/**
 * The backend's clients for one profile (capability-repos CR018), built from its endpoints and rebuilt when
 * they change: the relay's message and push-device clients and client compat (the relay's URL), the directory
 * (one stable switchable client: holders never see a freed one), registration, blobs and the locker (the
 * registration URL). A client whose endpoint did not change is kept. Signed requests use the signer once set.
 * The owner's thread.
 */
class BackendClientSet : public Module {
public:
  /** The endpoints to build for: empty ones fall back to `defaults` (the app's platform defaults). */
  struct Endpoints {
    ServiceEndpointConfig relay;
    DirectoryConfig directory;
    ServiceEndpointConfig registration;
  };

  explicit BackendClientSet(SwitchableDirectoryClient& directory);

  /** The endpoints an empty one falls back to (given by the app). */
  void SetDefaults(Endpoints defaults) { defaults_ = std::move(defaults); }
  /** Builds or rebuilds for `endpoints`; a missing endpoint leaves its clients out (null). */
  void Apply(const Endpoints& endpoints);
  /** Signs the relay, push-device, blob and locker requests (the profile's identity). */
  void SetSigner(RelayAuthSigner signer);
  /** Drops every client (the profile closes). */
  void Clear();

  IRelayClient* Relay() { return relay_.get(); }
  IPushDeviceClient* PushDevices() { return push_devices_.get(); }
  IClientCompatClient* ClientCompat() { return client_compat_.get(); }
  IRegistrationClient* Registration() { return registration_.get(); }
  IBlobClient* Blob() { return blob_.get(); }
  ILockerClient* Locker() { return locker_.get(); }

private:
  void ApplyRelay(const std::string& url);
  void ApplyDirectory(DirectoryConfig directory);
  void ApplyRegistration(const std::string& url);
  void WireSigner();

  SwitchableDirectoryClient& directory_;
  Endpoints defaults_;
  RelayAuthSigner signer_;
  std::string relay_url_;
  std::string directory_fingerprint_;
  std::string registration_url_;
  std::unique_ptr<HttpRelayClient> relay_;
  std::unique_ptr<HttpPushDeviceClient> push_devices_;
  std::unique_ptr<HttpClientCompatClient> client_compat_;
  std::unique_ptr<HttpRegistrationClient> registration_;
  std::unique_ptr<HttpBlobClient> blob_;
  std::unique_ptr<HttpLockerClient> locker_;
};

} // namespace pbr
