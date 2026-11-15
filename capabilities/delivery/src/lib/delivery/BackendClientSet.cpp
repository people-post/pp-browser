#include "lib/delivery/BackendClientSet.h"

#include "foundation/tls/CurlSsl.h"
#include "lib/net/RelayOrigins.h"
#include "lib/people/RegistrationStatus.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace pbr {

BackendClientSet::BackendClientSet(SwitchableDirectoryClient& directory) : directory_(directory) {
  redirectLogger("BackendClientSet");
}

namespace {

std::string OrDefault(const std::string& value, const std::string& fallback) {
  return value.empty() ? fallback : value;
}

/**
 * The directory the holders see: one left at the org's relay (its default, MR006) follows our preferred relay; one
 * configured apart from it stays.
 */
DirectoryConfig FollowingPreferred(DirectoryConfig directory, const std::string& org_relay,
                                   const std::string& preferred) {
  if (preferred.empty() || org_relay.empty()) {
    return directory;
  }
  const std::vector<ServiceEndpointConfig> providers = EffectiveDirectoryProviders(directory);
  const bool at_org = providers.size() == 1 && SameRelay(providers.front().base_url, org_relay) &&
                      (providers.front().transport.empty() || providers.front().transport == "http");
  if (!at_org) {
    return directory;
  }
  DirectoryConfig following;
  following.base_url = preferred;
  following.transport = "http";
  return following;
}

} // namespace

std::vector<std::string> BackendClientSet::OursFor(const Endpoints& endpoints) const {
  if (!endpoints.our_relays.empty()) {
    return BoundOurRelays(endpoints.our_relays);
  }
  return OurRelayOrigins(OrDefault(endpoints.relay.base_url, defaults_.relay.base_url), endpoints.extra_relays);
}

void BackendClientSet::Apply(const Endpoints& endpoints) {
  const std::string org_relay = OrDefault(endpoints.relay.base_url, defaults_.relay.base_url);
  const std::vector<std::string> ours = OursFor(endpoints);
  const std::string preferred = ours.empty() ? std::string() : ours.front();
  ApplyOrg(org_relay);
  ApplyPreferred(preferred);
  DirectoryConfig directory = endpoints.directory;
  if (directory.base_url.empty()) {
    directory.base_url = defaults_.directory.base_url;
  }
  ApplyDirectory(FollowingPreferred(std::move(directory), org_relay, preferred));
  ApplyHome(OrDefault(endpoints.registration.base_url, defaults_.registration.base_url));
  const size_t asked = endpoints.our_relays.empty() ? endpoints.extra_relays.size() + 1 : endpoints.our_relays.size();
  ApplyExtraRelays(ours, asked);
  WireBriefLlmKey();
  WireSender();
  WireDirectory();
}

void BackendClientSet::ApplyOrg(const std::string& url) {
  const std::string normalized = NormalizeRegistrationBackend(url);
  if (normalized.empty()) {
    client_compat_.reset();
    org_relay_url_.clear();
    return;
  }
  if (client_compat_ && org_relay_url_ == normalized) {
    return;
  }
  org_relay_url_ = normalized;
  client_compat_ = std::make_shared<HttpClientCompatClient>(url);
}

void BackendClientSet::WireBriefLlmKey() {
  // On every Apply: the org's relay may change while a registration client is kept (its relay's endpoint did not).
  const auto takes = [this](const std::string& url) {
    return !org_relay_url_.empty() && !url.empty() && SameRelay(url, org_relay_url_);
  };
  if (registration_) {
    registration_->SetTakesBriefLlmKey(takes(relay_url_));
  }
  for (const Extra& extra : extras_) {
    extra.registration->SetTakesBriefLlmKey(takes(extra.origin));
  }
}

void BackendClientSet::ApplyPreferred(const std::string& url) {
  if (url.empty()) {
    relay_.reset();
    push_devices_.reset();
    registration_.reset();
    blob_.reset();
    relay_url_.clear();
    log().warning << "no relay of ours (relay.base_url is empty); relay clients disabled";
    return;
  }
  if (relay_ && relay_url_ == url) {
    return;
  }
  relay_url_ = url;
  relay_ = std::make_shared<HttpRelayClient>(url);
  push_devices_ = std::make_shared<HttpPushDeviceClient>(url);
  registration_ = std::make_shared<HttpRegistrationClient>(url);
  blob_ = std::make_shared<HttpBlobClient>(url);
  const auto audience = AudienceOf(url); // set before the clients are handed out: never changed after
  relay_->SetAudience(audience);
  push_devices_->SetAudience(audience);
  registration_->SetAudience(audience);
  blob_->SetAudience(audience);
  WireSigner();
}

void BackendClientSet::ApplyDirectory(DirectoryConfig directory) {
  if (directory.base_url.empty() && directory.providers.empty()) {
    directory_fingerprint_.clear();
    configured_directory_.reset();
    directory_providers_.clear();
    log().warning << "directory.base_url is empty; directory client disabled";
    return;
  }
  NormalizeDirectoryConfig(directory);
  std::vector<std::unique_ptr<mesh::IDirectoryClient>> backends;
  std::vector<std::string> providers;
  std::string fingerprint;
  for (const ServiceEndpointConfig& provider : EffectiveDirectoryProviders(directory)) {
    const std::string transport = provider.transport.empty() ? "http" : provider.transport;
    if (transport != "http") {
      // Amp providers need the mesh: the mesh's owner asks them through its directory twin (N029 nd4).
      continue;
    }
    fingerprint.append(provider.base_url).push_back('|');
    providers.push_back(provider.base_url);
    backends.push_back(std::make_unique<HttpDirectoryClient>(provider.base_url, HttpTimeout{}, directory_cards_));
  }
  if (backends.empty()) {
    directory_fingerprint_.clear();
    configured_directory_.reset();
    directory_providers_.clear();
    log().warning << "directory providers empty; directory client disabled";
    return;
  }
  if (configured_directory_ && directory_fingerprint_ == fingerprint) {
    return;
  }
  directory_fingerprint_ = std::move(fingerprint);
  directory_providers_ = std::move(providers);
  if (backends.size() == 1) {
    configured_directory_ = std::shared_ptr<mesh::IDirectoryClient>(std::move(backends.front()));
  } else {
    configured_directory_ = std::make_shared<FailoverDirectoryClient>(std::move(backends));
  }
}

std::shared_ptr<mesh::IDirectoryClient> BackendClientSet::RelayDirectory(const std::string& origin) {
  for (const auto& [known, client] : relay_directories_) {
    if (known == origin) {
      return client;
    }
  }
  // Asked beside the configured one, so bounded tighter than its 30 s: a relay down must not hold a search long.
  return std::make_shared<HttpDirectoryClient>(origin, HttpTimeout{.total_s = 10, .connect_s = 5}, directory_cards_);
}

void BackendClientSet::WireDirectory() {
  if (!configured_directory_) {
    wired_directory_.clear();
    relay_directories_.clear();
    directory_.SetBackend(nullptr);
    return;
  }
  std::vector<OurRelaysDirectory::Source> sources{
      {.origin = directory_providers_.front(), .client = configured_directory_}};
  std::vector<std::pair<std::string, std::shared_ptr<mesh::IDirectoryClient>>> relays;
  std::string fingerprint = directory_fingerprint_ + (directory_worker_ ? "#worker" : "#");
  for (const OurRelay& relay : OurRelays()) {
    const bool named = std::any_of(directory_providers_.begin(), directory_providers_.end(),
                                   [&relay](const std::string& provider) { return SameRelay(provider, relay.origin); });
    // The preferred relay's directory is the configured one (its default): a directory configured apart from it
    // replaces it. Our other relays have no directory setting of their own: theirs is asked.
    if (!relay.registration || named) {
      continue;
    }
    auto client = RelayDirectory(relay.origin);
    relays.emplace_back(relay.origin, client);
    sources.push_back({.origin = relay.origin, .client = std::move(client)});
    fingerprint.append(relay.origin).push_back('|');
  }
  relay_directories_ = std::move(relays);
  if (directory_.Configured() && wired_directory_ == fingerprint) {
    return;
  }
  wired_directory_ = std::move(fingerprint);
  if (sources.size() == 1) {
    directory_.SetBackend(configured_directory_);
    return;
  }
  directory_.SetBackend(std::make_shared<OurRelaysDirectory>(std::move(sources), directory_worker_));
}

void BackendClientSet::SetDirectoryWorker(OurRelaysDirectory::RunElsewhere run_elsewhere) {
  directory_worker_ = std::move(run_elsewhere);
  WireDirectory();
}

void BackendClientSet::ApplyHome(const std::string& url) {
  if (url.empty()) {
    locker_.reset();
    home_url_.clear();
    return;
  }
  if (locker_ && home_url_ == url) {
    return;
  }
  home_url_ = url;
  locker_ = std::make_shared<HttpLockerClient>(url);
  locker_->SetAudience(AudienceOf(url)); // the org's origin (MR006), pinned whether or not it is one of ours
  WireSigner();
}

void BackendClientSet::ApplyExtraRelays(const std::vector<std::string>& origins, const size_t asked) {
  std::vector<Extra> next;
  for (size_t i = 1; i < origins.size(); ++i) {
    const std::string& origin = origins[i];
    const auto kept =
        std::find_if(extras_.begin(), extras_.end(), [&origin](const Extra& e) { return e.origin == origin; });
    if (kept != extras_.end()) {
      next.push_back(*kept); // unchanged: holders keep their clients
      continue;
    }
    Extra extra{.origin = origin,
                .relay = std::make_shared<HttpRelayClient>(origin),
                .push_devices = std::make_shared<HttpPushDeviceClient>(origin),
                .registration = std::make_shared<HttpRegistrationClient>(origin)};
    const auto audience = AudienceOf(origin);
    extra.relay->SetAudience(audience);
    extra.push_devices->SetAudience(audience);
    extra.registration->SetAudience(audience);
    next.push_back(std::move(extra));
  }
  if (!origins.empty() && origins.size() < asked) {
    log().warning << "our relays: " << (asked - origins.size())
                  << " left out (not a URL, a relay listed already, or past " << kMaxOurRelays << " in all)";
  }
  extras_ = std::move(next);
  WireSigner();
}

std::shared_ptr<IRelayClient> BackendClientSet::HistoryRelay() const {
  if (!home_url_.empty()) {
    for (const Extra& extra : extras_) {
      if (SameRelay(extra.origin, home_url_)) {
        return extra.relay;
      }
    }
  }
  return relay_;
}

std::vector<BackendClientSet::OurRelay> BackendClientSet::OurRelays() const {
  std::lock_guard lock(ours_mutex_);
  return ours_;
}

void BackendClientSet::WireSender() {
  std::vector<OurRelay> ours;
  if (relay_) {
    ours.push_back(OurRelay{.origin = NormalizeRegistrationBackend(relay_url_),
                            .relay = relay_,
                            .push_devices = push_devices_,
                            .registration = nullptr});
    for (const Extra& extra : extras_) {
      ours.push_back(OurRelay{.origin = extra.origin,
                              .relay = extra.relay,
                              .push_devices = extra.push_devices,
                              .registration = extra.registration});
    }
  }
  std::vector<RelaySendCoordinator::OurRelay> sending;
  for (const OurRelay& relay : ours) {
    sending.push_back({.origin = relay.origin, .client = relay.relay});
  }
  {
    std::lock_guard lock(ours_mutex_);
    ours_ = std::move(ours);
  }
  sender_.SetOurRelays(std::move(sending));
  if (pins_) {
    sender_.SetCardRelayCheck([pins = pins_](const std::string& origin, const std::string& peer_id) {
      return pins->CheckCardRelay(origin, peer_id);
    });
  } else {
    sender_.SetCardRelayCheck(nullptr);
  }
  // A relay not ours gets a client of its own, signed by us and with our send hooks (copies: set on this thread),
  // for the PeerId the recipient's card names; none named (a locator's relay, no card yet): the PeerId it presents,
  // never pinned (MR007: no one else's relay writes our pins).
  // Its origin came from a contact: held to a public host (IsPublicRelayOrigin), none is made for one outside it, and
  // its requests, the identity fetch included, go to public addresses only.
  sender_.SetForeignClients(
      [signer = signer_, hooks = send_hooks_, pins = pins_](const std::string& origin, const std::string& peer_id) {
        const bool lab = DevCaBundleActive();
        if (!IsPublicRelayOrigin(origin, lab)) {
          return std::shared_ptr<IRelayClient>();
        }
        auto client = std::make_shared<HttpRelayClient>(origin, !lab);
        if (signer) {
          client->SetAuthSigner(signer);
        }
        client->SetSendHooks(hooks);
        if (!peer_id.empty()) {
          client->SetAudience(std::make_shared<FixedRelayAudience>(peer_id));
        } else if (pins) {
          client->SetAudience(pins->ForeignAudienceFor(origin, !lab));
        }
        return std::shared_ptr<IRelayClient>(std::move(client));
      });
}

void BackendClientSet::SetPins(std::shared_ptr<RelayPins> pins) {
  pins_ = std::move(pins);
  // Clients keep the audience they were made with (work in flight reads it): the next Apply makes them anew.
  Clear();
}

std::shared_ptr<IRelayAudience> BackendClientSet::AudienceOf(const std::string& url) const {
  return pins_ && !url.empty() ? pins_->AudienceFor(url) : nullptr;
}

void BackendClientSet::SetSigner(RelayAuthSigner signer) {
  signer_ = std::move(signer);
  WireSigner();
  WireSender();
}

void BackendClientSet::SetRelaySendHooks(RelaySendHooks hooks) {
  send_hooks_ = std::move(hooks);
  if (relay_) {
    relay_->SetSendHooks(send_hooks_);
  }
  for (const Extra& extra : extras_) {
    extra.relay->SetSendHooks(send_hooks_);
  }
  WireSender();
}

void BackendClientSet::WireSigner() {
  if (relay_) {
    relay_->SetSendHooks(send_hooks_);
  }
  for (const Extra& extra : extras_) {
    extra.relay->SetSendHooks(send_hooks_);
    if (signer_) {
      extra.relay->SetAuthSigner(signer_);
      extra.push_devices->SetAuthSigner(signer_);
    }
  }
  if (!signer_) {
    return;
  }
  if (relay_) {
    relay_->SetAuthSigner(signer_);
  }
  if (push_devices_) {
    push_devices_->SetAuthSigner(signer_);
  }
  if (blob_) {
    blob_->SetAuthSigner(signer_);
  }
  if (locker_) {
    locker_->SetAuthSigner(signer_);
  }
}

void BackendClientSet::Clear() {
  relay_.reset();
  push_devices_.reset();
  client_compat_.reset();
  registration_.reset();
  blob_.reset();
  locker_.reset();
  extras_.clear();
  org_relay_url_.clear();
  relay_url_.clear();
  home_url_.clear();
  directory_fingerprint_.clear();
  configured_directory_.reset();
  directory_providers_.clear();
  relay_directories_.clear();
  wired_directory_.clear(); // the holders' directory stays until the next Apply rebuilds it, as before
  WireSender();
}

} // namespace pbr
