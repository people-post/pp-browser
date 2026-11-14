#include "lib/delivery/BackendClientSet.h"

#include "lib/delivery/RelayOrigins.h"
#include "lib/people/RegistrationStatus.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace pbr {

BackendClientSet::BackendClientSet(SwitchableDirectoryClient& directory) : directory_(directory) {
  redirectLogger("BackendClientSet");
}

void BackendClientSet::Apply(const Endpoints& endpoints) {
  ApplyRelay(endpoints.relay.base_url.empty() ? defaults_.relay.base_url : endpoints.relay.base_url);
  DirectoryConfig directory = endpoints.directory;
  if (directory.base_url.empty()) {
    directory.base_url = defaults_.directory.base_url;
  }
  ApplyDirectory(std::move(directory));
  ApplyRegistration(endpoints.registration.base_url.empty() ? defaults_.registration.base_url
                                                            : endpoints.registration.base_url);
  ApplyExtraRelays(endpoints.extra_relays);
  WireSender();
}

void BackendClientSet::ApplyRelay(const std::string& url) {
  if (url.empty()) {
    relay_.reset();
    push_devices_.reset();
    client_compat_.reset();
    relay_url_.clear();
    log().warning << "relay.base_url is empty; relay client disabled";
    return;
  }
  if (relay_ && relay_url_ == url) {
    return;
  }
  relay_url_ = url;
  relay_ = std::make_shared<HttpRelayClient>(url);
  push_devices_ = std::make_shared<HttpPushDeviceClient>(url);
  client_compat_ = std::make_unique<HttpClientCompatClient>(url);
  WireSigner();
}

void BackendClientSet::ApplyDirectory(DirectoryConfig directory) {
  if (directory.base_url.empty() && directory.providers.empty()) {
    directory_fingerprint_.clear();
    directory_.SetBackend(nullptr);
    log().warning << "directory.base_url is empty; directory client disabled";
    return;
  }
  NormalizeDirectoryConfig(directory);
  std::vector<std::unique_ptr<mesh::IDirectoryClient>> backends;
  std::string fingerprint;
  for (const ServiceEndpointConfig& provider : EffectiveDirectoryProviders(directory)) {
    const std::string transport = provider.transport.empty() ? "http" : provider.transport;
    if (transport != "http") {
      // Amp providers need the mesh: the mesh's owner asks them through its directory twin (N029 nd4).
      continue;
    }
    fingerprint.append(provider.base_url).push_back('|');
    backends.push_back(std::make_unique<HttpDirectoryClient>(provider.base_url));
  }
  if (backends.empty()) {
    directory_fingerprint_.clear();
    directory_.SetBackend(nullptr);
    log().warning << "directory providers empty; directory client disabled";
    return;
  }
  if (directory_.Configured() && directory_fingerprint_ == fingerprint) {
    return;
  }
  directory_fingerprint_ = std::move(fingerprint);
  if (backends.size() == 1) {
    directory_.SetBackend(std::shared_ptr<mesh::IDirectoryClient>(std::move(backends.front())));
  } else {
    directory_.SetBackend(std::make_shared<FailoverDirectoryClient>(std::move(backends)));
  }
}

void BackendClientSet::ApplyRegistration(const std::string& url) {
  if (url.empty()) {
    registration_.reset();
    blob_.reset();
    locker_.reset();
    registration_url_.clear();
    log().warning << "registration.base_url is empty; registration client disabled";
    return;
  }
  if (registration_ && registration_url_ == url) {
    return;
  }
  registration_url_ = url;
  registration_ = std::make_unique<HttpRegistrationClient>(url);
  blob_ = std::make_unique<HttpBlobClient>(url);
  locker_ = std::make_unique<HttpLockerClient>(url);
  WireSigner();
}

void BackendClientSet::ApplyExtraRelays(const std::vector<std::string>& urls) {
  std::vector<std::string> origins = OurRelayOrigins(relay_url_, urls);
  std::vector<Extra> next;
  for (size_t i = 1; i < origins.size(); ++i) {
    const std::string& origin = origins[i];
    const auto kept =
        std::find_if(extras_.begin(), extras_.end(), [&origin](const Extra& e) { return e.origin == origin; });
    if (kept != extras_.end()) {
      next.push_back(*kept); // unchanged: holders keep their clients
      continue;
    }
    next.push_back(Extra{.origin = origin,
                         .relay = std::make_shared<HttpRelayClient>(origin),
                         .push_devices = std::make_shared<HttpPushDeviceClient>(origin),
                         .registration = std::make_shared<HttpRegistrationClient>(origin)});
  }
  if (next.size() < urls.size()) {
    log().warning << "relay.extra_base_urls: " << (urls.size() - next.size())
                  << " left out (not a URL, a relay listed already, or past " << kMaxOurRelays << " in all)";
  }
  extras_ = std::move(next);
  WireSigner();
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
  // A relay not ours gets a client of its own, signed by us and with our send hooks (copies: set on this thread).
  sender_.SetForeignClients([signer = signer_, hooks = send_hooks_](const std::string& origin) {
    auto client = std::make_shared<HttpRelayClient>(origin);
    if (signer) {
      client->SetAuthSigner(signer);
    }
    client->SetSendHooks(hooks);
    return std::shared_ptr<IRelayClient>(std::move(client));
  });
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
  relay_url_.clear();
  directory_fingerprint_.clear();
  registration_url_.clear();
  WireSender();
}

} // namespace pbr
