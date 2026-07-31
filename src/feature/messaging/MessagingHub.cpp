#include "feature/messaging/MessagingHub.h"

#include "feature/messaging/GroupInviteGate.h"
#include "feature/messaging/GroupMembershipService.h"
#include "feature/messaging/RelayDirectoryKemKeyResolver.h"
#include "feature/messaging/RelayDirectorySigningKeyResolver.h"
#include "feature/messaging/SqlitePskSessionStore.h"

#include "feature/messaging/PushDeviceCoordinator.h"
#include "feature/ai/AgentSession.h"
#include "base/crypto/ProfileSecretsService.h"
#include "base/data/LlmPreset.h"
#include "base/error/AppError.h"
#include "base/data/Libp2pRole.h"
#include "base/data/SessionStore.h"
#include "base/data/UserPreferences.h"
#include "base/messaging/DirectChatTarget.h"
#include "base/messaging/GroupTypes.h"
#include "base/net/HttpClient.h"
#include "base/net/RegistrationClientUtil.h"
#include "base/people/ContactTypes.h"
#include "base/platform/Platform.h"
#include "base/data/PlatformDefaults.h"
#include "libp2p/integration/host/DialBackService.h"
#include "libp2p/integration/host/CircuitRelayService.h"
#include "libp2p/integration/host/MediaRelayService.h"
#include "base/people/MeshHopPolicy.h"
#include "libp2p/integration/host/NatTraversal.h"
#include "libp2p/integration/host/Reachability.h"
#include "common/StartupTiming.h"

#include <SDL3/SDL_timer.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <unordered_set>

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

std::string MaskBriefLlmApiKey(const std::string& key) {
  constexpr const char kPrefix[] = "brf_llm_";
  if (key.empty()) {
    return "";
  }
  if (key.size() <= sizeof(kPrefix) - 1 + 4) {
    return std::string(kPrefix) + "••••";
  }
  return key.substr(0, sizeof(kPrefix) - 1 + 4) + "••••";
}

std::string FormatExpiresDisplay(const std::string& iso) {
  if (iso.size() >= 10) {
    return iso.substr(0, 10);
  }
  return iso;
}

void FillRegistrationFields(ProfileIdentityView& view, const LocalIdentity& identity) {
  const RegistrationStatus status = ClassifyRegistration(identity);
  view.registered = identity.registered ? "yes" : "no";
  view.registration_status = RegistrationStatusLabel(status);
  view.registration_expires =
      identity.registration_expires_at.empty() ? "" : FormatExpiresDisplay(identity.registration_expires_at);
  view.register_label = RegistrationActionLabel(status);
  view.show_register = true;
  view.show_rotate = (status == RegistrationStatus::Active || status == RegistrationStatus::ExpiringSoon) &&
                     !identity.brief_llm_api_key.empty();
  view.brief_llm_key_masked = MaskBriefLlmApiKey(identity.brief_llm_api_key);
}

} // namespace

MessagingHub::MessagingHub() {
  redirectLogger("MessagingHub");
}

MessagingHub::~MessagingHub() {
  Shutdown();
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
    AppendIpv6ListenCandidatesForPreferred(libp2p_cfg.listen_multiaddr, runtime.listen_candidates);
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
      if (session_store_ && session_store_->IsInitialized()) {
        if (auto saved = session_store_->SaveConfig(config_); !saved) {
          log().warning << "Failed to persist libp2p listen multiaddr: " << saved.error().message;
        } else {
          log().info << "libp2p listening on " << bound;
        }
      } else {
        log().info << "libp2p listening on " << bound << " (session store not ready to persist)";
      }
    }
  }
  StartMeshServices(role);
  return {};
}

void MessagingHub::StartMeshServices(Libp2pRole role) {
  if (!node_runtime_ || !node_runtime_->IsRunning()) {
    return;
  }

  dial_back_ = std::make_unique<DialBackService>(*node_runtime_->Host(), *node_runtime_->Sessions());
  dial_back_->Start();

  circuit_relay_ = std::make_unique<CircuitRelayService>(*node_runtime_->Host(), *node_runtime_->Sessions());
  if (role == Libp2pRole::Node && config_.libp2p.capabilities.circuit_relay) {
    circuit_relay_->Start();
  }

  // Outbound client API always available; inbound hosting only when Node + capability.
  media_relay_ = std::make_unique<MediaRelayService>(*node_runtime_->Host(), *node_runtime_->Sessions());
  media_relay_->SetBudget(config_.libp2p.media_relay_budget);
  media_relay_->SetPricing(config_.libp2p.pricing.media_relay);
  if (role == Libp2pRole::Node && config_.libp2p.capabilities.media_relay) {
    media_relay_->Start();
  }

  ApplyMeshAdmissionPolicies();

  reachability_.SetOnUpdated([this]() {
    if (on_reachability_updated_) {
      on_reachability_updated_();
    }
  });

  if (role == Libp2pRole::Node) {
    const bool try_upnp = !upnp_auto_tried_;
    upnp_auto_tried_ = true;
    reachability_.StartProbe(*node_runtime_, *dial_back_, try_upnp);
  }
  WireCallMediaRelayDeps();
}

void MessagingHub::WireCallMediaRelayDeps() {
  if (!call_sessions_) {
    return;
  }
  media_relay_client_ = std::make_unique<MediaRelayServiceClient>(media_relay_.get());
  dial_registry_ = std::make_unique<PeerSessionDialRegistry>(
      node_runtime_ ? node_runtime_->Sessions() : nullptr);
  CallSessionManager::MediaRelayDeps deps;
  deps.relay = media_relay_client_.get();
  deps.dial = dial_registry_.get();
  Libp2pConfig libp2p = config_.libp2p;
  NormalizeLibp2pConfig(libp2p);
  deps.bootstrap_peers = libp2p.bootstrap_peers;
  deps.prefer_contacts = libp2p.prefer_contacts_for_routing;
  call_sessions_->SetMediaRelayDeps(std::move(deps));
}

void MessagingHub::ApplyMeshAdmissionPolicies() {
  const bool prefer = config_.libp2p.prefer_contacts_for_routing;
  const bool node = ResolveLibp2pRole(config_.libp2p) == Libp2pRole::Node;
  std::unordered_set<std::string> contact_ids;
  if (contacts_) {
    if (auto listed = contacts_->List()) {
      for (const std::string& id : ContactPeerIds(*listed)) {
        contact_ids.insert(id);
      }
    }
  }

  // Org-style: if Node with no contacts loaded, do not refuse strangers.
  const bool limit_strangers = prefer && node && !contact_ids.empty();

  if (circuit_relay_) {
    CircuitRelayAdmissionPolicy policy;
    policy.prefer_contacts_only = limit_strangers;
    policy.contact_peer_ids = contact_ids;
    circuit_relay_->SetAdmissionPolicy(std::move(policy));
  }
  if (media_relay_) {
    MediaRelayAdmissionPolicy policy;
    policy.prefer_contacts_only = limit_strangers;
    policy.contact_peer_ids = contact_ids;
    media_relay_->SetAdmissionPolicy(std::move(policy));
  }
}

void MessagingHub::StopLibp2p() {
  if (call_sessions_) {
    call_sessions_->SetMediaRelayDeps({});
  }
  media_relay_client_.reset();
  dial_registry_.reset();
  if (media_relay_) {
    media_relay_->Stop();
    media_relay_.reset();
  }
  if (circuit_relay_) {
    circuit_relay_->Stop();
    circuit_relay_.reset();
  }
  if (dial_back_) {
    dial_back_->Stop();
    dial_back_.reset();
  }
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
  ApplyMeshAdmissionPolicies();
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
  p2p_->SetProfileDataDir(data_dir_);
  group_membership_ = std::make_unique<GroupMembershipService>(*store_, *contacts_, *identity_, *group_roster_,
                                                               *group_invite_gate_, *p2p_);
  inbox_->SetGroupMembership(group_membership_.get());
  p2p_->SetGroupMembership(group_membership_.get());
  call_sessions_ = std::make_unique<CallSessionManager>(*store_, *contacts_, *identity_, *call_session_store_,
                                                       *call_media_keys_, *p2p_, *psk_store_, *call_media_engine_);
  p2p_->SetCallSessionManager(call_sessions_.get());
  call_sessions_->AbandonOrphanedCallsAfterRestart();
  WireCallMediaRelayDeps();
  actions_ = std::make_unique<ContactActionDispatcher>(*inbox_, *contacts_, *identity_, *store_,
                                                       group_membership_.get(), registration_, p2p_.get());

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
  p2p_->SetProfileDataDir(data_dir_);
  group_membership_ = std::make_unique<GroupMembershipService>(*store_, *contacts_, *identity_, *group_roster_,
                                                               *group_invite_gate_, *p2p_);
  inbox_->SetGroupMembership(group_membership_.get());
  p2p_->SetGroupMembership(group_membership_.get());
  call_sessions_ = std::make_unique<CallSessionManager>(*store_, *contacts_, *identity_, *call_session_store_,
                                                       *call_media_keys_, *p2p_, *psk_store_, *call_media_engine_);
  p2p_->SetCallSessionManager(call_sessions_.get());
  call_sessions_->AbandonOrphanedCallsAfterRestart();
  WireCallMediaRelayDeps();
  actions_ = std::make_unique<ContactActionDispatcher>(*inbox_, *contacts_, *identity_, *store_,
                                                       group_membership_.get(), registration_, p2p_.get());
  if (auto prefs = UserPreferences::LoadProfile(data_dir_); prefs) {
    const GroupInvitePolicy policy = GroupInvitePolicyFromString(prefs->group_invite_policy);
    group_invite_gate_->SetInboundPolicy(policy);
    group_membership_->SetInboundPolicy(policy);
  }
  RegisterContactEndpoints();
  if (agent_) {
    router_ = std::make_unique<MessageRouter>(*inbox_, *p2p_, *agent_, *store_);
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
    router_ = std::make_unique<MessageRouter>(*inbox_, *p2p_, agent, *store_);
  }
}

void MessagingHub::BindSessionStore(SessionStore& store) {
  session_store_ = &store;
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
  TickReachabilityUx();
}

ReachabilitySnapshot MessagingHub::Reachability() const {
  return reachability_.Snapshot();
}

void MessagingHub::SetOnReachabilityUpdated(std::function<void()> callback) {
  on_reachability_updated_ = std::move(callback);
}

void MessagingHub::RunReachabilityProbe(bool try_upnp) {
  if (!node_runtime_ || !dial_back_) {
    return;
  }
  reachability_.StartProbe(*node_runtime_, *dial_back_, try_upnp);
}

void MessagingHub::TryUpnpPortMapping() {
  if (!node_runtime_) {
    return;
  }
  const std::string bound = node_runtime_->BoundListenMultiaddr();
  if (ShouldSkipUpnpForListen(bound)) {
    return;
  }
  const auto port = TcpPortFromMultiaddr(bound);
  if (!port) {
    return;
  }
  (void)TryUpnpTcpPortMapping(*port);
  RunReachabilityProbe(false);
}

ProfileIdentityView MessagingHub::LoadProfileIdentityView() {
  ProfileIdentityView view;
  if (!IsInitialized() || !IsMessagingReady()) {
    return view;
  }
  auto identity = Identity().Get();
  if (!identity) {
    return view;
  }
  view.ready = true;
  view.nickname = identity->nickname;
  view.peer_id = identity->peer_id;
  view.relay_id = identity->relay_user_id;
  view.public_key_b64 = identity->public_key_b64;
  FillRegistrationFields(view, *identity);
  return view;
}

Roe<void> MessagingHub::SaveProfileNickname(const std::string& nickname) {
  if (!IsInitialized()) {
    return {};
  }
  auto identity = Identity().Get();
  if (!identity) {
    if (!IsMessagingReady()) {
      return {};
    }
    return identity.error();
  }
  if (nickname == identity->nickname) {
    return {};
  }
  if (!IsMessagingReady()) {
    return AppError::Pin(Err::Pin::Required, "Unlock profile PIN to save identity");
  }

  LocalIdentity updated = *identity;
  updated.nickname = nickname;
  if (auto saved = Identity().Update(updated); !saved) {
    return saved.error();
  }

  if (identity->registered) {
    auto result = UpdateRegisteredNickname(Registration(), Identity(), nickname);
    if (!result) {
      return result.error();
    }
  }
  return {};
}

Roe<void> MessagingHub::RegisterIdentity(const std::string& nickname) {
  if (!IsInitialized()) {
    return AppError::Pin(Err::Pin::Required, "Messaging hub not initialized");
  }
  if (!IsMessagingReady()) {
    return AppError::Pin(Err::Pin::Required, "Unlock profile PIN to register");
  }

  auto identity = Identity().Get();
  if (!identity) {
    return identity.error();
  }

  if (!nickname.empty()) {
    LocalIdentity updated = *identity;
    updated.nickname = nickname;
    if (auto saved = Identity().Update(updated); !saved) {
      return saved.error();
    }
    identity = Identity().Get();
    if (!identity) {
      return identity.error();
    }
  }

  auto applied = FinishAndPersistRegistration(
      Registration(), Identity(), identity->nickname,
      Libp2p() ? Libp2p()->ListenMultiaddrs() : std::vector<std::string>{});
  if (!applied) {
    return applied.error();
  }
  return {};
}

Roe<void> MessagingHub::RotateBriefLlmKey() {
  if (!IsInitialized()) {
    return AppError::Pin(Err::Pin::Required, "Messaging hub not initialized");
  }
  if (!IsMessagingReady()) {
    return AppError::Pin(Err::Pin::Required, "Unlock profile PIN to rotate API key");
  }

  auto identity = Identity().Get();
  if (!identity) {
    return identity.error();
  }
  if (identity->brief_llm_api_key.empty()) {
    return AppError::Auth(Err::Auth::NotRegistered, "No Brief API key yet")
        .WithUser("No Brief API key yet — register on the network in Me first.");
  }

  const AppConfig& config =
      (session_store_ && session_store_->IsInitialized()) ? session_store_->Snapshot().config : config_;
  std::string base_url = config.llm.base_url;
  if (ResolvePreset(config) != "brief" || base_url.empty()) {
    base_url = "https://www.brief.global/api/llm/v1";
  }
  while (!base_url.empty() && base_url.back() == '/') {
    base_url.pop_back();
  }
  const std::string url = base_url + "/keys/rotate";

  auto response = HttpClient::Post(url, "{}", {{"Authorization", "Bearer " + identity->brief_llm_api_key},
                                               {"Content-Type", "application/json"}});
  if (!response) {
    return response.error();
  }
  if (response->status_code == 401 || response->status_code == 403) {
    LocalIdentity expired = *identity;
    MarkRegistrationExpired(expired);
    (void)Identity().Update(expired);
    const char* renew_hint = response->status_code == 403
                                 ? "Registration expired — use Renew registration in Me → Profile."
                                 : "Brief API key rejected — renew registration in Me → Profile.";
    return AppError::Auth(Err::Auth::Forbidden,
                          "Brief key rotate HTTP " + std::to_string(response->status_code))
        .WithUser(renew_hint);
  }
  if (response->status_code < 200 || response->status_code >= 300) {
    auto json = nlohmann::json::parse(response->body, nullptr, false);
    std::string detail = "Brief API key rotate failed (HTTP " + std::to_string(response->status_code) + ")";
    if (!json.is_discarded() && json.contains("error")) {
      const auto& err = json["error"];
      if (err.is_string()) {
        detail = err.get<std::string>();
      } else if (err.is_object() && err.contains("message") && err["message"].is_string()) {
        detail = err["message"].get<std::string>();
      }
    }
    return AppError::Network(Err::Network::HttpError, detail);
  }

  auto root = nlohmann::json::parse(response->body, nullptr, false);
  if (root.is_discarded() || !root.contains("llm_api_key") || !root["llm_api_key"].is_string()) {
    return AppError::Auth(Err::Auth::Generic, "Brief API key rotate response missing llm_api_key")
        .WithUser("Couldn't update Brief API key — try Renew registration in Me → Profile.");
  }
  const std::string new_key = root["llm_api_key"].get<std::string>();
  if (new_key.empty()) {
    return AppError::Auth(Err::Auth::Generic, "Brief API key rotate returned empty key")
        .WithUser("Couldn't update Brief API key — try Renew registration in Me → Profile.");
  }

  LocalIdentity updated = *identity;
  updated.brief_llm_api_key = new_key;
  if (auto saved = Identity().Update(updated); !saved) {
    return saved.error();
  }
  return {};
}

void MessagingHub::TickReachabilityUx() {
  if (!config_.libp2p.node_enabled || Platform::IsMobile()) {
    return;
  }
  const ReachabilitySnapshot snap = reachability_.Snapshot();
  if (snap.status != ReachabilityStatus::OutboundOnly && snap.status != ReachabilityStatus::Blocked) {
    reachability_outbound_since_ms_ = 0;
    return;
  }
  const uint64_t now = SDL_GetTicks();
  if (reachability_outbound_since_ms_ == 0) {
    reachability_outbound_since_ms_ = now;
    return;
  }
  if (reachability_banner_shown_ || now - reachability_outbound_since_ms_ < 30000) {
    return;
  }
  reachability_banner_shown_ = true;
  if (on_reachability_updated_) {
    on_reachability_updated_();
  }
}

void MessagingHub::RefreshMeshCapabilities() {
  if (!messaging_ready_ || !node_runtime_) {
    return;
  }
  if (session_store_ && session_store_->IsInitialized()) {
    config_.libp2p = session_store_->Snapshot().config.libp2p;
  }
  if (circuit_relay_) {
    circuit_relay_->Stop();
    circuit_relay_.reset();
  }
  if (media_relay_) {
    media_relay_->Stop();
    media_relay_.reset();
  }
  media_relay_client_.reset();
  dial_registry_.reset();
  const Libp2pRole role = ResolveLibp2pRole(config_.libp2p);
  circuit_relay_ = std::make_unique<CircuitRelayService>(*node_runtime_->Host(), *node_runtime_->Sessions());
  if (role == Libp2pRole::Node && config_.libp2p.capabilities.circuit_relay) {
    circuit_relay_->Start();
  }
  media_relay_ = std::make_unique<MediaRelayService>(*node_runtime_->Host(), *node_runtime_->Sessions());
  media_relay_->SetBudget(config_.libp2p.media_relay_budget);
  media_relay_->SetPricing(config_.libp2p.pricing.media_relay);
  if (role == Libp2pRole::Node && config_.libp2p.capabilities.media_relay) {
    media_relay_->Start();
  }
  ApplyMeshAdmissionPolicies();
  WireCallMediaRelayDeps();
}

void MessagingHub::Apply(const NetworkConfig& next) {
  if (!initialized_) {
    return;
  }

  const bool service_urls_changed = next.relay.base_url != config_.relay.base_url ||
                                    next.directory.base_url != config_.directory.base_url ||
                                    next.registration.base_url != config_.registration.base_url;
  const bool mesh_changed =
      next.node_enabled != config_.libp2p.node_enabled ||
      next.circuit_relay != config_.libp2p.capabilities.circuit_relay ||
      next.media_relay != config_.libp2p.capabilities.media_relay ||
      next.prefer_contacts_for_routing != config_.libp2p.prefer_contacts_for_routing;

  config_.relay = next.relay;
  config_.directory = next.directory;
  config_.registration = next.registration;
  config_.libp2p.node_enabled = next.node_enabled;
  config_.libp2p.capabilities.circuit_relay = next.circuit_relay;
  config_.libp2p.capabilities.media_relay = next.media_relay;
  config_.libp2p.prefer_contacts_for_routing = next.prefer_contacts_for_routing;

  if (service_urls_changed) {
    UpdateServiceClients(config_);
    if (p2p_) {
      p2p_->SetRelayClient(relay_);
    }
    if (actions_) {
      actions_->SetRegistrationClient(registration_);
    }
  }
  if (PeerSessionManager* sessions = Sessions()) {
    sessions->SetConfig(SessionConfigFromApp(config_));
  }
  if (mesh_changed && messaging_ready_) {
    RefreshMeshCapabilities();
  }
}

void MessagingHub::Apply(const PolicyPrefs& prefs) {
  if (!initialized_) {
    return;
  }
  if (group_invite_gate_) {
    group_invite_gate_->SetInboundPolicy(prefs.group_invite_policy);
  }
  if (group_membership_) {
    group_membership_->SetInboundPolicy(prefs.group_invite_policy);
  }
}

void MessagingHub::Apply(const NotificationPrefs& prefs) {
  if (!initialized_ || !messaging_ready_) {
    return;
  }
  (void)PushDeviceCoordinator::SyncWithPreference(*this, prefs.show_notifications);
}

MessagingHub::NetworkConfig MessagingHub::ProjectNetwork(const AppConfig& config) {
  NetworkConfig out;
  out.relay = config.relay;
  out.directory = config.directory;
  out.registration = config.registration;
  out.node_enabled = config.libp2p.node_enabled;
  out.circuit_relay = config.libp2p.capabilities.circuit_relay;
  out.media_relay = config.libp2p.capabilities.media_relay;
  out.prefer_contacts_for_routing = config.libp2p.prefer_contacts_for_routing;
  return out;
}

MessagingHub::PolicyPrefs MessagingHub::ProjectPolicy(const ProfilePreferences& prefs) {
  return {.group_invite_policy = GroupInvitePolicyFromString(prefs.group_invite_policy)};
}

MessagingHub::NotificationPrefs MessagingHub::ProjectNotifications(const ProfilePreferences& prefs) {
  return {.show_notifications = prefs.show_notifications};
}

Roe<CircuitRelayBridgeResult> MessagingHub::RequestCircuitBridgePreferred(const std::string& target_multiaddr,
                                                                          int timeout_ms) {
  if (!circuit_relay_ || !node_runtime_ || !node_runtime_->Sessions()) {
    return Error("circuit-relay not available");
  }
  std::vector<Contact> contacts;
  if (contacts_) {
    if (auto listed = contacts_->List()) {
      contacts = std::move(*listed);
    }
  }
  Libp2pConfig libp2p = config_.libp2p;
  NormalizeLibp2pConfig(libp2p);
  auto hops = OrderCircuitHops(CollectContactHopCandidates(contacts), CollectSeedHopCandidates(libp2p.bootstrap_peers),
                               libp2p.prefer_contacts_for_routing);
  if (hops.empty()) {
    return Error("no circuit hop candidates");
  }

  CircuitRelayBridgeResult last;
  last.error = "all hops failed";
  PeerSessionManager& sessions = *node_runtime_->Sessions();
  for (const MeshHopCandidate& hop : hops) {
    const std::string key = hop.peer_id;
    if (!hop.multiaddr.empty()) {
      (void)sessions.RegisterEndpoint(key, hop.multiaddr);
      sessions.ClearDialBackoff(key);
    }
    if (!sessions.IsDialable(key)) {
      last.error = "hop not dialable: " + key;
      continue;
    }
    auto bridged = circuit_relay_->RequestBridge(key, target_multiaddr, timeout_ms);
    if (!bridged) {
      last.error = bridged.error().message;
      continue;
    }
    if (bridged->ok) {
      return *bridged;
    }
    last = *bridged;
  }
  return last;
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

  on_messaging_ready_ = nullptr;
  on_reachability_updated_ = nullptr;
  agent_ = nullptr;

  if (router_) {
    router_->SetOnLocalAction(nullptr);
    router_->SetSharedAiConfirmCallback(nullptr);
  }
  if (actions_) {
    actions_->SetOnActionMessage(nullptr);
  }
  if (inbox_) {
    inbox_->SetOnThreadChanged(nullptr);
  }
  if (p2p_) {
    p2p_->SetOnMessagesChanged(nullptr);
    p2p_->SetOnDeliveryNotice(nullptr);
    p2p_->SetOnBackgroundUnread(nullptr);
    p2p_->SetGroupMembership(nullptr);
  }
  if (inbox_) {
    inbox_->SetGroupMembership(nullptr);
  }
  if (call_sessions_) {
    call_sessions_->ClearMediaCallbacks();
  }

  router_.reset();
  store_->Flush();
  contacts_->Flush();
  if (messaging_ready_) {
    identity_->Flush();
  }
  actions_.reset();
  call_sessions_.reset();
  // Destroy P2P before groups — P2P held a non-owning Groups pointer.
  p2p_.reset();
  group_membership_.reset();
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
  http_push_devices_.reset();
  http_client_compat_.reset();
  relay_ = nullptr;
  push_devices_ = nullptr;
  client_compat_ = nullptr;
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
