#include "lib/delivery/BackendClientSet.h"

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
  relay_ = std::make_unique<HttpRelayClient>(url);
  push_devices_ = std::make_unique<HttpPushDeviceClient>(url);
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

void BackendClientSet::SetSigner(RelayAuthSigner signer) {
  signer_ = std::move(signer);
  WireSigner();
}

void BackendClientSet::WireSigner() {
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
  relay_url_.clear();
  directory_fingerprint_.clear();
  registration_url_.clear();
}

} // namespace pbr
