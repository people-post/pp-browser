#include "feature/messaging/MessagingHub.h"

#include "feature/messaging/RelayDirectoryKemKeyResolver.h"
#include "feature/messaging/RelayDirectorySigningKeyResolver.h"
#include "feature/messaging/SqlitePskSessionStore.h"

#include "feature/ai/AgentSession.h"
#include "base/crypto/ProfileSecretsService.h"
#include "base/messaging/DirectChatTarget.h"
#include "base/people/ContactTypes.h"
#include "base/platform/Platform.h"
#include "base/platform/PlatformDefaults.h"
#include "common/Logger.h"

#include <algorithm>
#include <chrono>
#include <filesystem>

namespace pbr {

namespace {

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
  const AppConfig defaults = PlatformDefaults::For(Platform::Detect());
  const std::string relay_url =
      config.relay.base_url.empty() ? defaults.relay.base_url : config.relay.base_url;
  const std::string directory_url =
      config.directory.base_url.empty() ? defaults.directory.base_url : config.directory.base_url;
  const std::string registration_url = config.registration.base_url.empty()
                                           ? defaults.registration.base_url
                                           : config.registration.base_url;

  if (!relay_url.empty()) {
    if (!http_relay_ || http_relay_url_ != relay_url) {
      http_relay_url_ = relay_url;
      http_relay_ = std::make_unique<HttpRelayClient>(http_relay_url_);
      WireRelayAuthSigner();
    }
    relay_ = http_relay_.get();
  } else {
    http_relay_.reset();
    http_relay_url_.clear();
    relay_ = nullptr;
    logging::getLogger("MessagingHub").warning << "relay.base_url is empty; relay client disabled";
  }

  if (!directory_url.empty()) {
    if (!http_directory_ || http_directory_url_ != directory_url) {
      http_directory_url_ = directory_url;
      http_directory_ = std::make_unique<HttpDirectoryClient>(http_directory_url_);
    }
    directory_ = http_directory_.get();
  } else {
    http_directory_.reset();
    http_directory_url_.clear();
    directory_ = nullptr;
    logging::getLogger("MessagingHub").warning << "directory.base_url is empty; directory client disabled";
  }

  if (!registration_url.empty()) {
    if (!http_registration_ || http_registration_url_ != registration_url) {
      http_registration_url_ = registration_url;
      http_registration_ = std::make_unique<HttpRegistrationClient>(http_registration_url_);
    }
    registration_ = http_registration_.get();
  } else {
    http_registration_.reset();
    http_registration_url_.clear();
    registration_ = nullptr;
    logging::getLogger("MessagingHub").warning
        << "registration.base_url is empty; registration client disabled";
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
    const DirectChatTarget target = DirectChatTargetFromContact(contact, ThreadChannel::E2ePublic);
    if (target.peer_identity_value.empty() || contact.multiaddrs.empty()) {
      continue;
    }
    for (const std::string& ma : contact.multiaddrs) {
      p2p_->RegisterPeerDirectEndpoint(target.peer_identity_value, ma);
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

  profile_id_ = std::filesystem::path(data_dir_).filename().string();
  if (profile_id_.empty()) {
    profile_id_ = "default";
  }

  store_ = std::make_unique<SqliteThreadStore>(data_dir_);
  contacts_ = std::make_unique<ContactsStore>(data_dir_);
  identity_ = std::make_unique<IdentityStore>(data_dir_, profile_id_);

  (void)store_->ReconcileOutbox();

  inbox_ = std::make_unique<InboxController>(*store_, *contacts_);
  (void)inbox_->CreateAiHomeThread();

  InstallServiceClients(config);

  psk_store_ = std::make_unique<SqlitePskSessionStore>(store_->ProfileDbPath(), profile_id_);
  ProfileSecretsService& secrets = ProfileSecretsService::Instance();
  secrets.RegisterDekConsumer(identity_.get());
  secrets.RegisterDekConsumer(psk_store_.get());
  signing_resolver_ = std::make_unique<RelayDirectorySigningKeyResolver>(signing_key_store_, *directory_);
  kem_resolver_ = std::make_unique<RelayDirectoryKemKeyResolver>(kem_key_store_, *directory_);

  // P2P stack without libp2p until profile unlock (relay-capable once identity exists).
  p2p_ = std::make_unique<P2pMessagingService>(*store_, *contacts_, *identity_, relay_, *inbox_,
                                                signing_key_store_, *signing_resolver_, kem_key_store_, *kem_resolver_,
                                                *psk_store_, nullptr, nullptr);
  actions_ = std::make_unique<ContactActionDispatcher>(*inbox_, *contacts_, *identity_, registration_, p2p_.get());

  messaging_ready_ = false;
  initialized_ = true;
  return {};
}

void MessagingHub::SetOnMessagingReady(std::function<void()> callback) {
  on_messaging_ready_ = std::move(callback);
}

void MessagingHub::NotifyMessagingReady() {
  if (on_messaging_ready_) {
    on_messaging_ready_();
  }
}

Roe<void> MessagingHub::BuildMessagingStack() {
  WireRelayAuthSigner();

  if (auto libp2p = StartLibp2p(config_); !libp2p) {
    logging::getLogger("MessagingHub").warning << "libp2p host start failed: " << libp2p.error().message;
  }

  p2p_ = std::make_unique<P2pMessagingService>(*store_, *contacts_, *identity_, relay_, *inbox_,
                                                signing_key_store_, *signing_resolver_, kem_key_store_, *kem_resolver_,
                                                *psk_store_, libp2p_host_.get(), peer_sessions_.get());
  RegisterContactEndpoints();
  actions_ = std::make_unique<ContactActionDispatcher>(*inbox_, *contacts_, *identity_, registration_, p2p_.get());
  if (agent_) {
    router_ = std::make_unique<MessageRouter>(*inbox_, *p2p_, *agent_);
  }
  return {};
}

Roe<void> MessagingHub::EnsureMessagingReady() {
  if (!initialized_) {
    return Error("Messaging hub not initialized");
  }
  if (messaging_ready_) {
    return {};
  }
  if (!ProfileSecretsService::Instance().IsUnlocked()) {
    return Error("Profile vault is locked");
  }
  if (auto identity = identity_->LoadOrCreate(); !identity) {
    return identity.error();
  } else {
    (void)identity;
  }

  if (auto built = BuildMessagingStack(); !built) {
    return built.error();
  }

  messaging_ready_ = true;
  NotifyMessagingReady();
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
  if (p2p_) {
    router_ = std::make_unique<MessageRouter>(*inbox_, *p2p_, agent);
  }
}

PeerSigningKeyStore& MessagingHub::SigningKeys() {
  return signing_key_store_;
}

void MessagingHub::TickLibp2p() {
  if (!messaging_ready_) {
    return;
  }
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
  if (messaging_ready_) {
    identity_->Flush();
  }
  actions_.reset();
  p2p_.reset();
  StopLibp2p();
  signing_resolver_.reset();
  kem_resolver_.reset();
  ProfileSecretsService& secrets = ProfileSecretsService::Instance();
  if (identity_) {
    secrets.UnregisterDekConsumer(identity_.get());
  }
  if (psk_store_) {
    secrets.UnregisterDekConsumer(psk_store_.get());
  }
  psk_store_.reset();
  inbox_.reset();
  http_relay_.reset();
  http_directory_.reset();
  http_registration_.reset();
  relay_ = nullptr;
  directory_ = nullptr;
  registration_ = nullptr;
  http_relay_url_.clear();
  http_directory_url_.clear();
  http_registration_url_.clear();
  identity_.reset();
  contacts_.reset();
  store_.reset();
  signing_key_store_.Clear();
  kem_key_store_.Clear();
  messaging_ready_ = false;
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
