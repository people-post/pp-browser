#include "feature/messaging/MessagingHub.h"

#include "feature/messaging/RelayDirectorySigningKeyResolver.h"
#include "feature/messaging/SqlitePskSessionStore.h"

#include "feature/ai/AgentSession.h"
#include "base/crypto/CryptoUtil.h"

#include <filesystem>

namespace pbr {

MessagingHub& MessagingHub::Instance() {
  static MessagingHub hub;
  return hub;
}

void MessagingHub::WireRelayAuthSigner() {
  if (!http_relay_ || !identity_) {
    return;
  }
  http_relay_->SetAuthSigner([this](const std::vector<uint8_t>& sign_bytes) -> Roe<std::string> {
    return identity_->SignBytes(sign_bytes);
  });
}

void MessagingHub::UpdateServiceClients(const AppConfig& config) {
  if (!config.relay.base_url.empty()) {
    if (!http_relay_ || http_relay_url_ != config.relay.base_url) {
      http_relay_url_ = config.relay.base_url;
      http_relay_ = std::make_unique<HttpRelayClient>(http_relay_url_);
      WireRelayAuthSigner();
    }
    relay_ = http_relay_.get();
  } else {
    if (!mock_relay_) {
      mock_relay_ = std::make_unique<MockRelayClient>();
      const auto test_private_key = HexToBytes(
          "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
      if (test_private_key) {
        mock_relay_->SetReplySigningPrivateKey(*test_private_key);
      }
    }
    relay_ = mock_relay_.get();
  }

  if (!config.directory.base_url.empty()) {
    if (!http_directory_ || http_directory_url_ != config.directory.base_url) {
      http_directory_url_ = config.directory.base_url;
      http_directory_ = std::make_unique<HttpDirectoryClient>(http_directory_url_);
    }
    directory_ = http_directory_.get();
  } else {
    if (!mock_directory_) {
      mock_directory_ = std::make_unique<MockDirectoryClient>();
    }
    directory_ = mock_directory_.get();
  }

  if (!config.registration.base_url.empty()) {
    if (!http_registration_ || http_registration_url_ != config.registration.base_url) {
      http_registration_url_ = config.registration.base_url;
      http_registration_ = std::make_unique<HttpRegistrationClient>(http_registration_url_);
    }
    registration_ = http_registration_.get();
  } else {
    if (!mock_registration_) {
      mock_registration_ = std::make_unique<MockRegistrationClient>();
    }
    registration_ = mock_registration_.get();
  }
}

void MessagingHub::InstallServiceClients(const AppConfig& config) {
  UpdateServiceClients(config);
}

Roe<void> MessagingHub::Initialize(const AppConfig& config, const std::string& profile_data_dir) {
  if (initialized_) {
    return {};
  }

  config_ = config;
  data_dir_ = profile_data_dir;
  std::error_code ec;
  std::filesystem::create_directories(data_dir_, ec);

  store_ = std::make_unique<SqliteThreadStore>(data_dir_);
  contacts_ = std::make_unique<ContactsStore>(data_dir_);
  identity_ = std::make_unique<IdentityStore>(data_dir_);

  if (auto identity = identity_->LoadOrCreate()) {
    (void)identity;
  }

  (void)store_->ReconcileOutbox();

  inbox_ = std::make_unique<InboxController>(*store_, *contacts_);
  (void)inbox_->CreateAiHomeThread();

  InstallServiceClients(config);

  psk_store_ = std::make_unique<SqlitePskSessionStore>(store_->ProfileDbPath());
  signing_resolver_ = std::make_unique<RelayDirectorySigningKeyResolver>(signing_key_store_, *directory_);

  p2p_ = std::make_unique<P2pMessagingService>(*store_, *contacts_, *identity_, relay_, *inbox_,
                                                signing_key_store_, *signing_resolver_, *psk_store_);
  actions_ = std::make_unique<ContactActionDispatcher>(*inbox_, *contacts_, *identity_, registration_, p2p_.get());

  initialized_ = true;
  return {};
}

Roe<void> MessagingHub::Reinitialize(const AppConfig& config, const std::string& profile_data_dir) {
  if (!initialized_) {
    return Initialize(config, profile_data_dir);
  }

  config_ = config;
  UpdateServiceClients(config);
  if (p2p_) {
    p2p_->SetRelayClient(relay_);
  }
  if (actions_) {
    actions_->SetRegistrationClient(registration_);
  }
  return {};
}

void MessagingHub::BindAgent(AgentSession& agent) {
  agent_ = &agent;
  router_ = std::make_unique<MessageRouter>(*inbox_, *p2p_, agent);
}

PeerSigningKeyStore& MessagingHub::SigningKeys() {
  return signing_key_store_;
}

void MessagingHub::Shutdown() {
  if (!initialized_) {
    return;
  }
  router_.reset();
  agent_ = nullptr;
  store_->Flush();
  contacts_->Flush();
  identity_->Flush();
  actions_.reset();
  p2p_.reset();
  signing_resolver_.reset();
  psk_store_.reset();
  inbox_.reset();
  http_relay_.reset();
  http_directory_.reset();
  http_registration_.reset();
  mock_relay_.reset();
  mock_directory_.reset();
  mock_registration_.reset();
  relay_ = nullptr;
  directory_ = nullptr;
  registration_ = nullptr;
  http_relay_url_.clear();
  http_directory_url_.clear();
  http_registration_url_.clear();
  identity_.reset();
  contacts_.reset();
  store_.reset();
  initialized_ = false;
}

InboxController& MessagingHub::Inbox() {
  return *inbox_;
}

P2pMessagingService& MessagingHub::P2p() {
  return *p2p_;
}

MessageRouter& MessagingHub::Router() {
  return *router_;
}

ContactActionDispatcher& MessagingHub::Actions() {
  return *actions_;
}

IThreadStore& MessagingHub::Store() {
  return *store_;
}

ContactsStore& MessagingHub::Contacts() {
  return *contacts_;
}

IdentityStore& MessagingHub::Identity() {
  return *identity_;
}

IDirectoryClient& MessagingHub::Directory() {
  return *directory_;
}

IRegistrationClient& MessagingHub::Registration() {
  return *registration_;
}

} // namespace pbr
