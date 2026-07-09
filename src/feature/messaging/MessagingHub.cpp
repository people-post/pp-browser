#include "feature/messaging/MessagingHub.h"

#include "feature/messaging/RelayDirectoryKemKeyResolver.h"
#include "feature/messaging/RelayDirectorySigningKeyResolver.h"
#include "feature/messaging/SqlitePskSessionStore.h"

#include "feature/ai/AgentSession.h"
#include "base/crypto/CryptoUtil.h"
#include "base/people/ContactTypes.h"
#include "base/platform/Platform.h"
#include "common/Logger.h"

#include <algorithm>
#include <chrono>
#include <filesystem>

namespace pbr {

namespace {

std::optional<std::string> PrimaryRelayIdFromContact(const Contact& contact) {
  for (const ContactId& id : contact.ids) {
    if (id.kind == ContactIdKind::RelayUser && id.primary) {
      return id.value;
    }
  }
  for (const ContactId& id : contact.ids) {
    if (id.kind == ContactIdKind::RelayUser) {
      return id.value;
    }
  }
  return std::nullopt;
}

PeerSessionConfig SessionConfigFromApp(const AppConfig& config) {
  PeerSessionConfig session;
  session.max_connections = config.libp2p.max_connections;
  session.max_concurrent_dials = config.libp2p.max_concurrent_dials;
  session.dial_timeout = std::chrono::milliseconds(config.libp2p.dial_timeout_ms);
  session.idle_ttl = std::chrono::milliseconds(config.libp2p.idle_ttl_ms);
  session.dial_failure_backoff = std::chrono::milliseconds(config.libp2p.dial_failure_backoff_ms);
  if (Platform::Detect() == PlatformKind::Android) {
    session.max_connections = std::min(session.max_connections, size_t{16});
    session.max_concurrent_dials = std::min(session.max_concurrent_dials, size_t{4});
    if (session.idle_ttl > std::chrono::milliseconds(120000)) {
      session.idle_ttl = std::chrono::milliseconds(120000);
    }
  }
  return session;
}

} // namespace

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

Roe<void> MessagingHub::StartLibp2p(const AppConfig& config) {
  StopLibp2p();
  libp2p_host_ = std::make_unique<Libp2pHost>();

  Libp2pHostConfig host_config;
  host_config.listen_multiaddr = config.libp2p.listen_multiaddr;
  if (auto priv = identity_->GetEd25519PrivateKey()) {
    host_config.ed25519_private_key = *priv;
  }
  if (auto pub = identity_->GetEd25519PublicKey()) {
    host_config.ed25519_public_key = *pub;
  }

  auto started = libp2p_host_->Start(host_config);
  if (!started) {
    libp2p_host_.reset();
    return started.error();
  }

  peer_sessions_ = std::make_unique<PeerSessionManager>(*libp2p_host_, SessionConfigFromApp(config));
  return {};
}

void MessagingHub::StopLibp2p() {
  peer_sessions_.reset();
  if (libp2p_host_) {
    libp2p_host_->Stop();
    libp2p_host_.reset();
  }
}

void MessagingHub::RegisterContactEndpoints() {
  if (!p2p_ || !contacts_) {
    return;
  }
  auto listed = contacts_->List();
  if (!listed) {
    return;
  }
  for (const Contact& contact : *listed) {
    auto relay_id = PrimaryRelayIdFromContact(contact);
    if (!relay_id || contact.multiaddrs.empty()) {
      continue;
    }
    for (const std::string& ma : contact.multiaddrs) {
      p2p_->RegisterPeerDirectEndpoint(*relay_id, ma);
    }
  }
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
  kem_resolver_ = std::make_unique<RelayDirectoryKemKeyResolver>(kem_key_store_, *directory_);

  if (auto libp2p = StartLibp2p(config); !libp2p) {
    // Direct transport unavailable; messaging still works via relay.
    logging::getLogger("MessagingHub").warning << "libp2p host start failed: " << libp2p.error().message;
  }

  p2p_ = std::make_unique<P2pMessagingService>(*store_, *contacts_, *identity_, relay_, *inbox_,
                                                signing_key_store_, *signing_resolver_, kem_key_store_, *kem_resolver_,
                                                *psk_store_, libp2p_host_.get(), peer_sessions_.get());
  RegisterContactEndpoints();
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
  if (peer_sessions_) {
    peer_sessions_->SetConfig(SessionConfigFromApp(config));
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

void MessagingHub::TickLibp2p() {
  if (peer_sessions_) {
    peer_sessions_->Tick();
  }
  if (p2p_) {
    p2p_->TickLibp2p();
  }
}

void MessagingHub::SuspendLibp2pColdPeers() {
  if (peer_sessions_) {
    peer_sessions_->SuspendColdPeers();
  }
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
  StopLibp2p();
  signing_resolver_.reset();
  kem_resolver_.reset();
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

Libp2pHost* MessagingHub::Libp2p() {
  return libp2p_host_.get();
}

PeerSessionManager* MessagingHub::Sessions() {
  return peer_sessions_.get();
}

} // namespace pbr
