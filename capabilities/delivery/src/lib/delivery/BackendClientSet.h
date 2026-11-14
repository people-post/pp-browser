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
 * they change. A client whose endpoint did not change is kept. Signed requests use the signer once set.
 *
 * Two origins (multi-relay MR006): the **org's** (the config's `relay` / `registration`, the deployment's) and **our
 * relays**, preferred first (My relays, the profile's; by default the org's relay and `relay.extra_base_urls`). The
 * preferred relay carries the relay services: messages and push (Relay, PushDevices), registration, profile photos,
 * blobs and chat files (Blob); the directory asks it first unless one is configured apart. The org's keeps client
 * compatibility (ClientCompat) and the home of what is not moved yet: account lockers (Locker) and stream history
 * (HistoryRelay, while that relay is ours). A registration client keeps the Brief LLM key only on the org's relay.
 *
 * Our other relays get a message, push-device and registration client each (OurRelays). The directory asks the
 * configured one and then each other relay of ours it does not name (OurRelaysDirectory): a name search all at once, a
 * lookup in order; the cards its lookups carry go to DirectoryCards. Sends go through one stable RelaySendCoordinator
 * (Sender): into the recipient's relays in list order, with a client of its own for a relay not ours. The owner's
 * thread.
 */
class BackendClientSet : public Module {
public:
  /** The endpoints to build for: empty ones fall back to `defaults` (the app's platform defaults). */
  struct Endpoints {
    /** The org's relay (config `relay`): client compatibility, and the first of our relays by default. */
    ServiceEndpointConfig relay;
    /** The directory configured: the org's relay's by default, which then follows our preferred relay. */
    DirectoryConfig directory;
    /** The org's registration URL (config `registration`): the home of account lockers and stream history. */
    ServiceEndpointConfig registration;
    /** Our relays' base URLs, preferred first (My relays); empty: `relay`, then `extra_relays`. BoundOurRelays. */
    std::vector<std::string> our_relays;
    /** Our other relays' base URLs (config `relay.extra_base_urls`), after `relay`'s: only when `our_relays` is empty. */
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
  /** The org's relay base URL `endpoints` name (its default when empty). */
  std::string OrgRelayOf(const Endpoints& endpoints) const {
    return endpoints.relay.base_url.empty() ? defaults_.relay.base_url : endpoints.relay.base_url;
  }
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

  /** Shared: work in flight keeps the client it started with when a change replaces it (MR006). */
  std::shared_ptr<IRelayClient> Relay() const { return relay_; }
  /** Where the outbox sends: by the recipient's relay list (stable for this set's life). */
  RelaySendCoordinator& Sender() { return sender_; }
  /** Ours, preferred first (none while no relay is configured). A copy: any thread (a push binding's worker). */
  std::vector<OurRelay> OurRelays() const;
  /** The preferred relay's (shared, as Relay). */
  std::shared_ptr<IPushDeviceClient> PushDevices() const { return push_devices_; }
  /** The org's (MR006; shared, as Relay). */
  std::shared_ptr<IClientCompatClient> ClientCompat() const { return client_compat_; }
  /** The preferred relay's (shared, as Relay). */
  std::shared_ptr<IRegistrationClient> Registration() const { return registration_; }
  /** The preferred relay's: profile photos and chat files (shared, as Relay). */
  std::shared_ptr<IBlobClient> Blob() const { return blob_; }
  /** The org's registration URL's: lockers stay where they were written (MR006; shared, as Relay). */
  std::shared_ptr<ILockerClient> Locker() const { return locker_; }
  /** Stream history: the relay of ours at the org's registration URL, where it was kept; else the preferred one. */
  std::shared_ptr<IRelayClient> HistoryRelay() const;
  /** The org's relay base URL (normalized; empty when none). */
  const std::string& OrgRelay() const { return org_relay_url_; }

private:
  /** Ours, preferred first, for `endpoints` (BoundOurRelays). */
  std::vector<std::string> OursFor(const Endpoints& endpoints) const;
  /** The org's: client compatibility. */
  void ApplyOrg(const std::string& url);
  /** The preferred relay's: messages, push, registration, blobs. */
  void ApplyPreferred(const std::string& url);
  void ApplyDirectory(DirectoryConfig directory);
  /** The org's registration URL: lockers. */
  void ApplyHome(const std::string& url);
  void ApplyExtraRelays(const std::vector<std::string>& origins, size_t asked);
  /** Each registration client of ours keeps the Brief LLM key only when its relay is the org's (every Apply). */
  void WireBriefLlmKey();
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
  std::string org_relay_url_;
  std::string relay_url_;
  std::string home_url_;
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
  std::shared_ptr<HttpRelayClient> relay_;
  std::shared_ptr<HttpPushDeviceClient> push_devices_;
  std::shared_ptr<HttpClientCompatClient> client_compat_;
  std::shared_ptr<HttpRegistrationClient> registration_;
  std::shared_ptr<HttpBlobClient> blob_;
  std::shared_ptr<HttpLockerClient> locker_;
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
