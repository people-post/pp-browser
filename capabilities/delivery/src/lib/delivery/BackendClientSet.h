#pragma once

#include "lib/delivery/HttpRelayClient.h"
#include "lib/delivery/OurRelaysDirectory.h"
#include "lib/delivery/RelaySendCoordinator.h"
#include "lib/net/HttpBlobClient.h"
#include "lib/net/HttpLockerClient.h"
#include "lib/net/OrgBackendClientsImpl.h"
#include "lib/net/SwitchableDirectoryClient.h"
#include "common/Module.h"
#include "common/ServiceEndpoints.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

/**
 * The backend's clients for one profile (capability-repos CR018), built from its endpoints and rebuilt when
 * they change: the relay's message and push-device clients and client compat (the relay's URL), the directory
 * (one stable switchable client: holders never see a freed one), registration, blobs and the locker (the
 * registration URL). A client whose endpoint did not change is kept. Signed requests use the signer once set.
 *
 * Our other relays (multi-relay R2, `relay.extra_base_urls`) get a message, push-device and registration client each
 * (OurRelays); blobs, files, lockers, profile photos and stream history stay the preferred relay's. The directory
 * asks the configured one (by default the preferred relay's) and then each other relay of ours it does not name
 * (OurRelaysDirectory): a name search all at once, a lookup in order; the cards its lookups carry go to DirectoryCards.
 * Sends go through one stable RelaySendCoordinator (Sender): into the recipient's relays in list order, with a client
 * of its own for a relay not ours. The owner's thread.
 */
class BackendClientSet : public Module {
public:
  /** The endpoints to build for: empty ones fall back to `defaults` (the app's platform defaults). */
  struct Endpoints {
    ServiceEndpointConfig relay;
    DirectoryConfig directory;
    ServiceEndpointConfig registration;
    /** Our other relays' base URLs (multi-relay R2), after the preferred one; bounded by OurRelayOrigins. */
    std::vector<std::string> extra_relays;
  };

  /** One relay of ours: its origin and the clients that speak for us there. */
  struct OurRelay {
    std::string origin;
    std::shared_ptr<IRelayClient> relay;
    std::shared_ptr<IPushDeviceClient> push_devices;
    /** The other relays' own; the preferred relay's is Registration() (the registration URL). */
    std::shared_ptr<IRegistrationClient> registration;
  };

  explicit BackendClientSet(SwitchableDirectoryClient& directory);

  /** The endpoints an empty one falls back to (given by the app). */
  void SetDefaults(Endpoints defaults) { defaults_ = std::move(defaults); }
  /** Builds or rebuilds for `endpoints`; a missing endpoint leaves its clients out (null). */
  void Apply(const Endpoints& endpoints);
  /** Signs the relay, push-device, blob and locker requests (the profile's identity). */
  void SetSigner(RelayAuthSigner signer);
  /** The relay sends' grants and our key (MR003 / MR005), for the relay client now and after a rebuild. */
  void SetRelaySendHooks(RelaySendHooks hooks);
  /** Where a name search asks our other relays' directories (a worker); none: in turn, on the searching thread. */
  void SetDirectoryWorker(OurRelaysDirectory::RunElsewhere run_elsewhere);
  /** Gets the account cards the directory's lookups carry (stable for this set's life). */
  DirectoryCardSink& DirectoryCards() { return *directory_cards_; }
  /** Drops every client (the profile closes). */
  void Clear();

  IRelayClient* Relay() { return relay_.get(); }
  /** Where the outbox sends: by the recipient's relay list (stable for this set's life). */
  RelaySendCoordinator& Sender() { return sender_; }
  /** Ours, preferred first (none while no relay is configured). A copy: any thread (a push binding's worker). */
  std::vector<OurRelay> OurRelays() const;
  IPushDeviceClient* PushDevices() { return push_devices_.get(); }
  IClientCompatClient* ClientCompat() { return client_compat_.get(); }
  IRegistrationClient* Registration() { return registration_.get(); }
  IBlobClient* Blob() { return blob_.get(); }
  ILockerClient* Locker() { return locker_.get(); }

private:
  void ApplyRelay(const std::string& url);
  void ApplyDirectory(DirectoryConfig directory);
  void ApplyRegistration(const std::string& url);
  void ApplyExtraRelays(const std::vector<std::string>& urls);
  /** The directory the holders see: the configured one, then the directory of each other relay of ours it does not name. */
  void WireDirectory();
  /** The directory client of our relay at `origin`, kept while it stays ours. */
  std::shared_ptr<mesh::IDirectoryClient> RelayDirectory(const std::string& origin);
  void WireSigner();
  /** Ours for the sender (preferred first) and its factory for the others, after any change. */
  void WireSender();

  SwitchableDirectoryClient& directory_;
  Endpoints defaults_;
  RelayAuthSigner signer_;
  RelaySendHooks send_hooks_;
  std::string relay_url_;
  std::string directory_fingerprint_;
  /** The configured directory (its providers, failing over) and its providers' base URLs. */
  std::shared_ptr<mesh::IDirectoryClient> configured_directory_;
  std::vector<std::string> directory_providers_;
  /** Our relays' own directories (those the configured one does not name), by origin. */
  std::vector<std::pair<std::string, std::shared_ptr<mesh::IDirectoryClient>>> relay_directories_;
  /** What the holders' directory was built from; unchanged: kept. */
  std::string wired_directory_;
  OurRelaysDirectory::RunElsewhere directory_worker_;
  std::shared_ptr<DirectoryCardSink> directory_cards_ = std::make_shared<DirectoryCardSink>();
  std::string registration_url_;
  std::shared_ptr<HttpRelayClient> relay_;
  std::shared_ptr<HttpPushDeviceClient> push_devices_;
  std::unique_ptr<HttpClientCompatClient> client_compat_;
  std::unique_ptr<HttpRegistrationClient> registration_;
  std::unique_ptr<HttpBlobClient> blob_;
  std::unique_ptr<HttpLockerClient> locker_;
  /** One other relay's clients (its origin as configured, bounded). */
  struct Extra {
    std::string origin;
    std::shared_ptr<HttpRelayClient> relay;
    std::shared_ptr<HttpPushDeviceClient> push_devices;
    std::shared_ptr<HttpRegistrationClient> registration;
  };
  std::vector<Extra> extras_;
  /** Our relays, preferred first (the preferred's clients are the ones above), as OurRelays gives them. */
  mutable std::mutex ours_mutex_;
  std::vector<OurRelay> ours_;
  RelaySendCoordinator sender_;
};

} // namespace pbr
