#include "feature/messaging/MessagingHub.h"

#include "feature/messaging/GroupInviteGate.h"
#include "feature/messaging/GroupMembershipService.h"
#include "feature/messaging/RelayDirectoryKemKeyResolver.h"
#include "feature/messaging/RelayDirectorySigningKeyResolver.h"
#include "feature/messaging/SqlitePskSessionStore.h"

#include "feature/ai/AgentSession.h"
#include "base/crypto/ProfileSecretsService.h"
#include "base/error/AppError.h"
#include "base/data/Libp2pRole.h"
#include "base/data/SessionStore.h"
#include "base/data/UserPreferences.h"
#include "base/messaging/DirectChatTarget.h"
#include "base/messaging/GroupTypes.h"
#include "base/people/ContactTypes.h"
#include "base/platform/Platform.h"
#include "base/data/PlatformDefaults.h"
#include "common/StartupTiming.h"

#include <algorithm>
#include <chrono>
#include <filesystem>

namespace pbr {

namespace {

PeerSessionConfig SessionConfigFromApp(const AppConfig& config) {
  PeerSessionConfig session = MakePeerSessionConfig(
      config.libp2p.max_connections, config.libp2p.max_concurrent_dials, config.libp2p.dial_timeout_ms,
      config.libp2p.idle_ttl_ms, config.libp2p.dial_failure_backoff_ms);
  if (Platform::IsMobile()) {
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

MessagingHub::MessagingHub() {
  redirectLogger("MessagingHub");
}

void MessagingHub::WireRelayAuthSigner() {
  if (!identity_) {
    return;
  }
  auto signer = [this](const std::vector<uint8_t>& sign_bytes) -> Roe<std::string> {
    return identity_->SignBytes(sign_bytes);
  };
  if (http_relay_) {
    http_relay_->SetAuthSigner(signer);
  }
  if (http_push_devices_) {
    http_push_devices_->SetAuthSigner(signer);
  }
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
      http_push_devices_ = std::make_unique<HttpPushDeviceClient>(http_relay_url_);
      http_client_compat_ = std::make_unique<HttpClientCompatClient>(http_relay_url_);
      WireRelayAuthSigner();
    }
    relay_ = http_relay_.get();
    push_devices_ = http_push_devices_.get();
    client_compat_ = http_client_compat_.get();
  } else {
    http_relay_.reset();
    http_push_devices_.reset();
    http_client_compat_.reset();
    http_relay_url_.clear();
    relay_ = nullptr;
    push_devices_ = nullptr;
    client_compat_ = nullptr;
    log().warning << "relay.base_url is empty; relay client disabled";
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
    log().warning << "directory.base_url is empty; directory client disabled";
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
    log().warning << "registration.base_url is empty; registration client disabled";
  }
}

void MessagingHub::InstallServiceClients(const AppConfig& config) {
  UpdateServiceClients(config);
}

Roe<void> MessagingHub::StartLibp2p(const AppConfig& config) {
  StartupPhase phase("MessagingHub::StartLibp2p");
  StopLibp2p();
  libp2p_last_error_.clear();

  Libp2pConfig libp2p_cfg = config.libp2p;
  NormalizeLibp2pConfig(libp2p_cfg);
  const Libp2pRole role = ResolveLibp2pRole(libp2p_cfg);

  NodeRuntimeConfig runtime;
  runtime.host.listen_enabled = (role == Libp2pRole::Node);
  runtime.host.listen_multiaddr = libp2p_cfg.listen_multiaddr;
  if (auto priv = identity_->GetEd25519PrivateKey()) {
    runtime.host.ed25519_private_key = *priv;
  }
  if (auto pub = identity_->GetEd25519PublicKey()) {
    runtime.host.ed25519_public_key = *pub;
  }
  runtime.sessions = SessionConfigFromApp(config_);
  runtime.bootstrap_peers = libp2p_cfg.bootstrap_peers;
  if (runtime.host.listen_enabled) {
    runtime.listen_candidates =
        BuildLibp2pListenCandidates(libp2p_cfg.listen_multiaddr, ListenBusyPolicy::DesktopFallback);
  }

  node_runtime_ = std::make_unique<NodeRuntime>();
  auto started = node_runtime_->Start(runtime);
  if (!started) {
    libp2p_last_error_ = node_runtime_->LastError().empty() ? started.error().message : node_runtime_->LastError();
    node_runtime_.reset();
    return started.error();
  }

  if (runtime.host.listen_enabled) {
    const std::string bound = node_runtime_->BoundListenMultiaddr();
    if (!bound.empty() && bound != config_.libp2p.listen_multiaddr) {
      config_.libp2p.listen_multiaddr = bound;
      if (SessionStore::Instance().IsInitialized()) {
        if (auto saved = SessionStore::Instance().SaveConfig(config_); !saved) {
          log().warning << "Failed to persist libp2p listen multiaddr: " << saved.error().message;
        } else {
          log().info << "libp2p listening on " << bound;
        }
      } else {
        log().info << "libp2p listening on " << bound << " (session store not ready to persist)";
      }
    }
  }
  return {};
}

void MessagingHub::StopLibp2p() {
  if (node_runtime_) {
    node_runtime_->Stop();
    node_runtime_.reset();
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

  {
    StartupPhase phase("MessagingHub::ReconcileOutbox");
    (void)store_->ReconcileOutbox();
  }

  InstallServiceClients(config);

  psk_store_ = std::make_unique<SqlitePskSessionStore>(store_->ProfileDbPath(), profile_id_);
  group_roster_ = std::make_unique<GroupRosterStore>(store_->ProfileDbPath());
  call_session_store_ = std::make_unique<CallSessionStore>(store_->ProfileDbPath());
  call_media_keys_ = std::make_unique<CallMediaKeyStore>(store_->ProfileDbPath(), profile_id_);
  call_media_engine_ = std::make_unique<CallMediaEngine>();
  group_invite_gate_ = std::make_unique<GroupInviteGate>(*contacts_, *group_roster_);
  directory_shadows_ = std::make_unique<DirectoryShadowCache>(*directory_);
  peer_labels_ = std::make_unique<PeerDisplayResolver>(*contacts_, *directory_shadows_, group_roster_.get());
  inbox_ = std::make_unique<InboxController>(*store_, *contacts_, *peer_labels_, directory_shadows_.get());
  directory_shadows_->SetOnUpdated([this]() {
    if (inbox_) {
      inbox_->NotifyThreadChanged();
    }
  });

  ProfileSecretsService& secrets = ProfileSecretsService::Instance();
  secrets.RegisterDekConsumer(identity_.get());
  secrets.RegisterDekConsumer(psk_store_.get());
  secrets.RegisterDekConsumer(call_media_keys_.get());
  signing_resolver_ = std::make_unique<RelayDirectorySigningKeyResolver>(signing_key_store_, *directory_);
  kem_resolver_ = std::make_unique<RelayDirectoryKemKeyResolver>(kem_key_store_, *directory_);

  // P2P stack without libp2p until profile unlock (relay-capable once identity exists).
  p2p_ = std::make_unique<P2pMessagingService>(*store_, *contacts_, *identity_, relay_, *inbox_,
                                                signing_key_store_, *signing_resolver_, kem_key_store_, *kem_resolver_,
                                                *psk_store_, *group_roster_, group_invite_gate_.get(), nullptr, nullptr);
  group_membership_ = std::make_unique<GroupMembershipService>(*store_, *contacts_, *identity_, *group_roster_,
                                                               *group_invite_gate_, *p2p_);
  call_sessions_ = std::make_unique<CallSessionManager>(*store_, *contacts_, *identity_, *call_session_store_,
                                                       *call_media_keys_, *p2p_, *psk_store_, *call_media_engine_);
  p2p_->SetCallSessionManager(call_sessions_.get());
  actions_ = std::make_unique<ContactActionDispatcher>(*inbox_, *contacts_, *identity_, registration_, p2p_.get());

  if (auto prefs = UserPreferences::LoadProfile(data_dir_); prefs) {
    group_invite_gate_->SetInboundPolicy(GroupInvitePolicyFromString(prefs->group_invite_policy));
    if (group_membership_) {
      group_membership_->SetInboundPolicy(GroupInvitePolicyFromString(prefs->group_invite_policy));
    }
  }

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
    log().warning << "libp2p host start failed: " << libp2p.error().message
                  << " (direct P2P unavailable; relay messaging may still work)";
  }

  p2p_ = std::make_unique<P2pMessagingService>(*store_, *contacts_, *identity_, relay_, *inbox_,
                                                signing_key_store_, *signing_resolver_, kem_key_store_, *kem_resolver_,
                                                *psk_store_, *group_roster_, group_invite_gate_.get(), Libp2p(),
                                                Sessions());
  group_membership_ = std::make_unique<GroupMembershipService>(*store_, *contacts_, *identity_, *group_roster_,
                                                               *group_invite_gate_, *p2p_);
  call_sessions_ = std::make_unique<CallSessionManager>(*store_, *contacts_, *identity_, *call_session_store_,
                                                       *call_media_keys_, *p2p_, *psk_store_, *call_media_engine_);
  p2p_->SetCallSessionManager(call_sessions_.get());
  if (auto prefs = UserPreferences::LoadProfile(data_dir_); prefs) {
    const GroupInvitePolicy policy = GroupInvitePolicyFromString(prefs->group_invite_policy);
    group_invite_gate_->SetInboundPolicy(policy);
    group_membership_->SetInboundPolicy(policy);
  }
  RegisterContactEndpoints();
  actions_ = std::make_unique<ContactActionDispatcher>(*inbox_, *contacts_, *identity_, registration_, p2p_.get());
  if (agent_) {
    router_ = std::make_unique<MessageRouter>(*inbox_, *p2p_, *agent_);
  }
  return {};
}

Roe<void> MessagingHub::EnsureMessagingReady() {
  StartupPhase phase("MessagingHub::EnsureMessagingReady");
  if (!initialized_) {
    return AppError::Pin(Err::Pin::Required, "Messaging hub not initialized");
  }
  if (messaging_ready_) {
    return {};
  }
  if (!ProfileSecretsService::Instance().IsUnlocked()) {
    return AppError::Pin(Err::Pin::Required, "Profile vault is locked");
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
  if (PeerSessionManager* sessions = Sessions()) {
    sessions->SetConfig(SessionConfigFromApp(config));
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
  if (node_runtime_) {
    node_runtime_->Tick();
  }
  if (p2p_) {
    p2p_->TickLibp2p();
  }
}

void MessagingHub::SuspendLibp2pColdPeers() {
  if (node_runtime_) {
    node_runtime_->SuspendColdPeers();
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
  if (call_sessions_) {
    call_sessions_->ClearMediaCallbacks();
  }
  call_sessions_.reset();
  group_membership_.reset();
  p2p_.reset();
  group_invite_gate_.reset();
  group_roster_.reset();
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
  if (call_media_keys_) {
    secrets.UnregisterDekConsumer(call_media_keys_.get());
  }
  call_media_engine_.reset();
  call_media_keys_.reset();
  call_session_store_.reset();
  psk_store_.reset();
  inbox_.reset();
  peer_labels_.reset();
  directory_shadows_.reset();
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

GroupMembershipService& MessagingHub::Groups() {
  return *group_membership_;
}

CallSessionManager* MessagingHub::Calls() {
  return call_sessions_.get();
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

DirectoryShadowCache& MessagingHub::DirectoryShadows() {
  return *directory_shadows_;
}

PeerDisplayResolver& MessagingHub::PeerLabels() {
  return *peer_labels_;
}

IRegistrationClient& MessagingHub::Registration() {
  return *registration_;
}

IPushDeviceClient* MessagingHub::PushDevices() {
  return push_devices_;
}

IClientCompatClient* MessagingHub::ClientCompat() {
  return client_compat_;
}

Libp2pHost* MessagingHub::Libp2p() {
  return node_runtime_ ? node_runtime_->Host() : nullptr;
}

PeerSessionManager* MessagingHub::Sessions() {
  return node_runtime_ ? node_runtime_->Sessions() : nullptr;
}

} // namespace pbr
