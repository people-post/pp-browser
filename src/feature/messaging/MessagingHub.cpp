#include "feature/messaging/MessagingHub.h"

#include "feature/messaging/ContactReachability.h"
#include "feature/messaging/GroupInviteGate.h"
#include "feature/messaging/AttachmentDownloadService.h"
#include "feature/messaging/GroupMembershipService.h"
#include "feature/messaging/RelayDirectoryKemKeyResolver.h"
#include "feature/messaging/RelayDirectorySigningKeyResolver.h"
#include "feature/messaging/SqlitePskSessionStore.h"

#include "feature/messaging/PushDeviceCoordinator.h"
#include "feature/ai/AgentSession.h"
#include "base/crypto/ProfileSecretsService.h"
#include "base/data/LlmPreset.h"
#include "base/platform/DeploymentProfile.h"
#include "base/error/AppError.h"
#include "base/data/Libp2pRole.h"
#include "base/data/SessionStore.h"
#include "base/data/UserPreferences.h"
#include "base/messaging/AttachmentCache.h"
#include "base/messaging/DirectChatTarget.h"
#include "base/messaging/ChatPayloadCodec.h"
#include "base/messaging/GroupTypes.h"
#include "base/messaging/SendRelayOptions.h"
#include "base/net/AttachmentClientUtil.h"
#include "base/net/BlobQuotaUtil.h"
#include "base/net/HttpClient.h"
#include "base/net/ProfileIconClientUtil.h"
#include "base/net/ProfileIconFetchUtil.h"
#include "base/net/RegistrationClientUtil.h"
#include "base/people/ProfileIconCache.h"
#include "base/platform/ProfileIconImagePrep.h"
#include "base/people/ContactIdentity.h"
#include "base/people/ContactTypes.h"
#include "base/runtime/AppLifecycle.h"
#include "base/runtime/BackgroundSyncScheduler.h"
#include "base/runtime/AppRuntime.h"
#include "base/platform/NetworkConnectivity.h"
#include "base/platform/Platform.h"
#include "base/data/PlatformDefaults.h"
#include "base/p2p/CircuitBridgeTarget.h"
#include "base/p2p/CircuitRelayTypes.h"
#include "base/p2p/MediaRelayTypes.h"
#include "base/p2p/CircuitTunnelCoordinator.h"
#include "base/p2p/LanMdnsDiscovery.h"
#include "base/p2p/SettledWait.h"
#include "base/people/MeshHopPolicy.h"
#include "base/mesh/link/AdpMultiaddr.h"
#include "base/p2p/NatTraversal.h"
#include "base/p2p/Reachability.h"
#include "base/runtime/StartupTiming.h"
#include "common/Utilities.h"

#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <unordered_set>

#include "common/ValueJson.h"
#include "common/PbrCompat.h"

namespace pbr {

namespace {

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
  // Always own an (empty) CallStack so CallUiBackend can bind CallStackRef() at construction,
  // even before Initialize builds the call session/media objects.
  call_stack_ = std::make_unique<CallStack>();
}

CallStackDeps MessagingHub::MakeCallStackDeps() {
  CallStackDeps deps;
  deps.store = store_.get();
  deps.contacts = contacts_.get();
  deps.identity = identity_.get();
  deps.psk = psk_store_.get();
  deps.p2p = p2p_.get();
  deps.mesh = [this]() { return mesh_.get(); };
  deps.config = [this]() -> const AppConfig& { return config_; };
  deps.prefetch_peer_reachability = [this](const std::string& identity) {
    PrefetchPeerReachability(identity);
  };
  deps.sync_mobile_ephemeral_listen = [this]() { SyncMobileEphemeralListen(); };
  deps.note_lan_mdns_peer_id = [this](const std::string& peer_id) {
    lan_mdns_contact_peer_ids_.insert(peer_id);
  };
  return deps;
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
  if (http_blob_) {
    http_blob_->SetAuthSigner(signer);
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
      http_blob_ = std::make_unique<HttpBlobClient>(http_registration_url_);
      WireRelayAuthSigner();
    }
    registration_ = http_registration_.get();
    blob_ = http_blob_.get();
  } else {
    http_registration_.reset();
    http_blob_.reset();
    http_registration_url_.clear();
    registration_ = nullptr;
    blob_ = nullptr;
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

  MeshHostConfig mesh_cfg;
  if (auto priv = identity_->GetDeviceMlDsaPrivateKey()) {
    mesh_cfg.host.device_ml_dsa_private_key = *priv;
  }
  if (auto pub = identity_->GetDeviceMlDsaPublicKey()) {
    mesh_cfg.host.device_ml_dsa_public_key = *pub;
  }
  mesh_cfg.host_circuit_relay = role == Libp2pRole::Node && config_.libp2p.capabilities.circuit_relay;
  mesh_cfg.host_media_relay = role == Libp2pRole::Node && config_.libp2p.capabilities.media_relay;
  mesh_cfg.media_relay_budget = config_.libp2p.media_relay_budget;
  mesh_cfg.media_relay_pricing = config_.libp2p.pricing.media_relay;
  mesh_cfg.start_reachability_probe = role == Libp2pRole::Node;
  if (role == Libp2pRole::Node) {
    mesh_cfg.try_upnp_first = !upnp_auto_tried_;
    upnp_auto_tried_ = true;
  }
  mesh_cfg.on_reachability_updated = [this]() {
    ApplyMeshAdmissionPolicies();
    PublishNodeAdvertisedAddrs();
    RegisterContactEndpoints();
    if (on_reachability_updated_) {
      on_reachability_updated_();
    }
  };
  mesh_cfg.mesh_enabled = libp2p_cfg.mesh_enabled && mesh_cfg.host.device_ml_dsa_private_key &&
                          mesh_cfg.host.device_ml_dsa_public_key;
  mesh_cfg.amp_udp_port =
      libp2p_cfg.amp_udp_port <= 0 ? 0 : static_cast<uint16_t>(libp2p_cfg.amp_udp_port);
  mesh_cfg.bootstrap_peers = libp2p_cfg.bootstrap_peers;

  mesh_ = std::make_unique<MeshHost>();
  auto started = mesh_->Start(mesh_cfg);
  if (!started) {
    libp2p_last_error_ = mesh_->LastError().empty() ? started.error().message : mesh_->LastError();
    mesh_.reset();
    return started.error();
  }
  if (mesh_cfg.mesh_enabled) {
    log().info << "amp stack listen=" << mesh_->AmpListenMultiaddr();
  } else {
    log().info << "mesh disabled (mesh_enabled=false); peer mesh underlay off";
  }
  StartMeshServices();
  return {};
}

void MessagingHub::StartMeshServices() {
  if (!mesh_ || !mesh_->IsRunning()) {
    return;
  }

  lan_mdns_ = std::make_unique<LanMdnsDiscovery>();
  lan_mdns_->SetOnDiscovered([this](const LanMdnsDiscoveredPeer& peer) { OnLanMdnsPeerDiscovered(peer); });
  if (auto started = lan_mdns_->Start(); !started) {
    log().warning << "LAN mDNS discovery unavailable: " << started.error().message;
  }

  ApplyMeshAdmissionPolicies();
  call_stack_->OnMeshServicesStarted();
  PublishNodeAdvertisedAddrs();
  SyncLanMdnsAdvertisement();
}

void MessagingHub::PublishNodeAdvertisedAddrs() {
  // A017: Identify/Host advertisement retired. Amp peers learn via mDNS / pasted ADP MAs.
}



void MessagingHub::PublishMobileCallScopedAddrs() {
  // A017: TCP ephemeral Identify publish retired; Amp listen is always advertised via mDNS.
}



void MessagingHub::SyncMobileEphemeralListen() {
  if (!Platform::IsMobile() || !messaging_ready_) {
    return;
  }
  // D10/A017: Amp UDP accept is always on — TCP ephemeral listen retired. Refresh mDNS for LAN.
  if (mesh_ && mesh_->Amp()) {
    SyncLanMdnsAdvertisement();
  }
}



void MessagingHub::SyncLanMdnsAdvertisement() {
  if (!lan_mdns_ || !lan_mdns_->IsRunning()) {
    return;
  }

  const bool amp_up = mesh_ && mesh_->Amp() && !mesh_->AmpListenMultiaddr().empty();
  if (!amp_up) {
    return;
  }

  const Libp2pRole role = ResolveLibp2pRole(config_.libp2p);
  const bool node_listen = role == Libp2pRole::Node;
  const bool ephemeral = Platform::IsMobile() && call_stack_ && call_stack_->WantEphemeralListen();
  if (!node_listen && !ephemeral) {
    lan_mdns_->SetAdvertisement({}, 0, {});
    return;
  }

  const std::string peer_id = mesh_->Amp()->LocalPeerId();
  int amp_udp = 0;
  if (auto parsed = amp::ParseAdpMultiaddr(mesh_->AmpListenMultiaddr())) {
    amp_udp = static_cast<int>(parsed->endpoint.port);
  }
  if (peer_id.empty() || amp_udp <= 0) {
    lan_mdns_->SetAdvertisement({}, 0, {});
    return;
  }

  // Empty host_ips → LanMdnsDiscovery enumerates LAN IFs (Amp typically binds 0.0.0.0).
  lan_mdns_->SetAdvertisement(peer_id, amp_udp, {});
}


void MessagingHub::OnLanMdnsPeerDiscovered(const LanMdnsDiscoveredPeer& peer) {
  AppRuntime::PostWorkerNormal([this, peer]() {
    if (peer.peer_id_base58.empty()) {
      return;
    }
    if (lan_mdns_contact_peer_ids_.find(peer.peer_id_base58) == lan_mdns_contact_peer_ids_.end()) {
      return;
    }
    auto ma = LanMdnsDiscovery::BuildMultiaddr(peer);
    auto adp = LanMdnsDiscovery::BuildAdpMultiaddr(peer);
    if (!ma && !adp) {
      return;
    }
    const std::string probe_ma = adp ? *adp : (ma ? *ma : "");
    const std::string mdns_ip = IpHostFromMultiaddrPrefix(probe_ma);
    if (IsLikelyUndialableLanIpv4(mdns_ip)) {
      log().info << "LAN mDNS skip undialable peer=" << peer.peer_id_base58 << " ma=" << probe_ma;
      return;
    }
    if (!(p2p_ && (adp || ma))) {
      return;
    }
    log().info << "LAN mDNS discovered peer=" << peer.peer_id_base58
               << " ma=" << (ma ? *ma : "") << " adp=" << (adp ? *adp : "");
    if (p2p_) {
      if (ma) {
        p2p_->RegisterPeerDirectEndpoint(peer.peer_id_base58, *ma);
      }
      if (adp) {
        log().info << "LAN mDNS amp dial peer=" << peer.peer_id_base58 << " ma=" << *adp;
        p2p_->RegisterPeerDirectEndpoint(peer.peer_id_base58, *adp);
      }
    }
    if (contacts_) {
      if (auto listed = contacts_->List(); listed) {
        for (const Contact& contact : *listed) {
          if (PeerIdFromContact(contact) != peer.peer_id_base58) {
            continue;
          }
          const DirectChatTarget target =
              DirectChatTargetFromContact(contact, ThreadChannel::E2ePublic);
          if (target.peer_identity_value.empty() ||
              target.peer_identity_value == peer.peer_id_base58) {
            continue;
          }
          if (p2p_) {
            if (ma) {
              p2p_->RegisterPeerDirectEndpoint(target.peer_identity_value, *ma);
            }
            if (auto adp2 = LanMdnsDiscovery::BuildAdpMultiaddr(peer)) {
              p2p_->RegisterPeerDirectEndpoint(target.peer_identity_value, *adp2);
            }
          }
          log().info << "LAN mDNS dial alias peer=" << peer.peer_id_base58
                        << " dial_key=" << target.peer_identity_value;
        }
      }
    }
  });
}


void MessagingHub::ApplyMeshAdmissionPolicies() {
  const bool prefer = config_.libp2p.prefer_contacts_for_routing;
  const bool node = ResolveLibp2pRole(config_.libp2p) == Libp2pRole::Node;
  // A017: Amp listen is always on; treat mobile in-call as link-scope hosting.
  const bool mobile_ephemeral = Platform::IsMobile() && call_stack_ && call_stack_->WantEphemeralListen();
  std::unordered_set<std::string> contact_ids;
  if (contacts_) {
    if (auto listed = contacts_->List()) {
      for (const std::string& id : ContactPeerIds(*listed)) {
        contact_ids.insert(id);
      }
    }
  }

  MeshReachabilityClass reach_class = MeshReachabilityClass::Unknown;
  switch (Reachability().status) {
  case ReachabilityStatus::Reachable:
    reach_class = MeshReachabilityClass::Reachable;
    break;
  case ReachabilityStatus::OutboundOnly:
    reach_class = MeshReachabilityClass::OutboundOnly;
    break;
  case ReachabilityStatus::Blocked:
    reach_class = MeshReachabilityClass::Blocked;
    break;
  case ReachabilityStatus::Unknown:
  case ReachabilityStatus::Checking:
  default:
    reach_class = MeshReachabilityClass::Unknown;
    break;
  }

  RelayScopeMask serve_mask = ProviderServeScopeMask(reach_class, node);
  const bool force_limit_strangers =
      node && (reach_class == MeshReachabilityClass::OutboundOnly ||
               reach_class == MeshReachabilityClass::Blocked);
  const bool limit_strangers =
      (prefer && node && !contact_ids.empty()) || force_limit_strangers;
  if (!limit_strangers && node) {
    serve_mask |= static_cast<RelayScopeMask>(RelayScope::Public);
  }

  if (mesh_ && mesh_->AmpCircuitTunnel()) {
    CircuitRelayAdmissionPolicy policy;
    policy.prefer_contacts_only = limit_strangers;
    policy.serve_scope_mask = serve_mask;
    policy.contact_peer_ids = contact_ids;
    mesh_->AmpCircuitTunnel()->SetAdmissionPolicy(std::move(policy));
  }
  if (mesh_ && mesh_->AmpMediaRelayCoord()) {
    MediaRelayAdmissionPolicy policy;
    if (mobile_ephemeral) {
      policy.prefer_contacts_only = true;
      policy.serve_scope_mask = kRelayScopeLinkSiteSocial;
      policy.contact_peer_ids = contact_ids;
    } else {
      policy.prefer_contacts_only = limit_strangers;
      policy.serve_scope_mask = serve_mask;
      policy.contact_peer_ids = contact_ids;
    }
    mesh_->AmpMediaRelayCoord()->SetAdmissionPolicy(std::move(policy));
  }
}


void MessagingHub::StopLibp2p() {
  mobile_ephemeral_start_inflight_ = false;
  mobile_ephemeral_start_inflight_at_ms_ = 0;
  mobile_ephemeral_stop_inflight_ = false;
  mobile_ephemeral_last_start_error_.clear();
  // Call-media teardown before the mesh stops. The bridge PrepareForTeardown is bracketed by
  // mesh circuit-request aborts so the Connect worker observes abort and unblocks (same order
  // as before the CallStack split).
  call_stack_->PrepareForMeshStop([this]() {
    if (mesh_) {
      mesh_->AbortInflightCircuitRequests();
    }
  });
  if (lan_mdns_) {
    lan_mdns_->Stop();
    lan_mdns_.reset();
  }
  // MeshHost::Stop tears down media_relay, circuit, dial-back, and runtime (in that order).
  // Keep bridge + dial registry alive until the libp2p host joins its workers — inbound
  // CallMediaKey wait and OpenStream completions may still touch them.
  if (mesh_) {
    mesh_->Stop();
    mesh_.reset();
  }
  call_stack_->FinishMeshStop();
}

void MessagingHub::AbortCallMediaForShutdown() {
  call_stack_->AbortCallMediaForShutdown();
}

void MessagingHub::RegisterContactEndpoints() {
  if (!p2p_ || !contacts_) {
    return;
  }
  auto listed = contacts_->List();
  if (!listed) {
    return;
  }
  lan_mdns_contact_peer_ids_.clear();
  for (const Contact& contact : *listed) {
    p2p_->RegisterContactDirectEndpoints(contact);
    const DirectChatTarget target = DirectChatTargetFromContact(contact, ThreadChannel::E2ePublic);
    if (target.peer_identity_value.empty()) {
      continue;
    }
    for (const std::string& ma : contact.multiaddrs) {
      p2p_->RegisterPeerDirectEndpoint(target.peer_identity_value, ma);
    }
    const std::vector<std::string> peer_ids = PeerIdsFromContact(contact);
    for (const std::string& peer_id : peer_ids) {
      lan_mdns_contact_peer_ids_.insert(peer_id);
    }
  }
  ApplyMeshAdmissionPolicies();
}


bool MessagingHub::IsContactReachable(const Contact& contact) const {
  return IsContactReachableForMessaging(contact, relay_ != nullptr);
}


void MessagingHub::PrefetchPeerReachability(const std::string& identity) {
  if (!messaging_ready_ || identity.empty() || !p2p_) {
    return;
  }
  // Warm Brief route cache for Account→relay: (non-contact call accept / leave).
  if (directory_shadows_ && IsAccountIdentityValue(identity)) {
    directory_shadows_->EnsureLookup(identity);
  }
  // A017: warm Amp endpoints only — no PeerSessionManager EnsureConnection.
  std::string peer_id;
  if (contacts_) {
    if (auto hit = contacts_->FindByIdentity(identity, ContactIdKind::Account)) {
      if (hit->has_value()) {
        peer_id = PeerIdFromContact(**hit);
      }
    }
    if (peer_id.empty()) {
      if (auto hit = contacts_->FindByIdentity(identity, ContactIdKind::RelayUser)) {
        if (hit->has_value()) {
          peer_id = PeerIdFromContact(**hit);
        }
      }
    }
    if (peer_id.empty()) {
      if (auto hit = contacts_->FindByIdentity(identity, ContactIdKind::PeerId)) {
        if (hit->has_value()) {
          peer_id = PeerIdFromContact(**hit);
        }
      }
    }
  }
  if (peer_id.empty()) {
    peer_id = identity;
  }
  (void)peer_id;
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
  initiation_billing_ = std::make_unique<InitiationBillingStore>(data_dir_);
  (void)initiation_billing_->Load();

  {
    StartupPhase phase("MessagingHub::ReconcileOutbox");
    (void)store_->ReconcileOutbox();
  }

  InstallServiceClients(config);

  psk_store_ = std::make_unique<SqlitePskSessionStore>(store_->ProfileDbPath(), profile_id_);
  group_roster_ = std::make_unique<GroupRosterStore>(store_->ProfileDbPath());
  if (auto stores = call_stack_->InitializeStores(store_->ProfileDbPath(), profile_id_); !stores) {
    return stores.error();
  }
  group_invite_gate_ = std::make_unique<GroupInviteGate>(*contacts_, *group_roster_);
  directory_shadows_ = std::make_unique<DirectoryShadowCache>(*directory_);
  peer_labels_ = std::make_unique<PeerDisplayResolver>(*contacts_, *directory_shadows_, group_roster_.get());
  inbox_ = std::make_unique<InboxController>(*store_, *contacts_, *peer_labels_, directory_shadows_.get());
  directory_shadows_->SetOnUpdated([this]() {
    if (inbox_) {
      inbox_->NotifyThreadChanged();
    }
  });
  directory_shadows_->SetOnHitCached([this](const DirectoryHit& hit) {
    EnsureDirectoryHitIconCached(hit);
    if (!initiation_billing_) {
      return;
    }
    std::string person_id;
    if (auto account = PrimaryAccountIdFromHit(hit)) {
      person_id = *account;
    } else {
      for (const ContactId& id : hit.ids) {
        if (id.kind == ContactIdKind::RelayUser && !id.value.empty()) {
          person_id = id.value;
          break;
        }
      }
      if (person_id.empty()) {
        person_id = hit.hit_id;
      }
    }
    if (!person_id.empty()) {
      (void)initiation_billing_->SetFloor(person_id, hit.initiation_floor);
    }
  });

  if (secrets_ != nullptr) {
    secrets_->RegisterDekConsumer(identity_.get());
    secrets_->RegisterDekConsumer(psk_store_.get());
    secrets_->RegisterDekConsumer(static_cast<SqliteThreadStore*>(store_.get()));
    secrets_->RegisterDekConsumer(call_stack_->MediaKeys());
  }
  signing_resolver_ = std::make_unique<RelayDirectorySigningKeyResolver>(signing_key_store_, *directory_);
  kem_resolver_ = std::make_unique<RelayDirectoryKemKeyResolver>(kem_key_store_, *directory_);

  // P2P stack without libp2p until profile unlock (relay-capable once identity exists).
  p2p_ = std::make_unique<P2pMessagingService>(*store_, *contacts_, *identity_, relay_, *inbox_,
                                                signing_key_store_, *signing_resolver_, kem_key_store_, *kem_resolver_,
                                                *psk_store_, *group_roster_, group_invite_gate_.get());
  p2p_->SetProfileDataDir(data_dir_);
  p2p_->SetInitiationBillingStore(initiation_billing_.get());
  p2p_->SetPeerRouteSources(directory_shadows_.get(), directory_);
  WireAttachmentDownloads();
  group_membership_ = std::make_unique<GroupMembershipService>(*store_, *contacts_, *identity_, *group_roster_,
                                                               *group_invite_gate_, *p2p_);
  inbox_->SetGroupMembership(group_membership_.get());
  p2p_->SetGroupMembership(group_membership_.get());
  call_stack_->BuildSessions(MakeCallStackDeps());
  if (auto* calls = call_stack_->Calls()) {
    calls->SetInitiationBillingStore(initiation_billing_.get());
  }
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

void MessagingHub::SetOnCallWake(std::function<void()> callback) {
  on_call_wake_ = std::move(callback);
}

void MessagingHub::NotifyMessagingReady() {
  if (!on_messaging_ready_) {
    return;
  }
  // EnsureMessagingReady may run on the IO thread (PIN unlock); UI bindings must run on UI.
  if (AppRuntime::CurrentlyOnUI()) {
    on_messaging_ready_();
    return;
  }
  AppRuntime::PostUI([this]() {
    if (on_messaging_ready_) {
      on_messaging_ready_();
    }
  });
}

Roe<void> MessagingHub::BuildMessagingStack() {
  WireRelayAuthSigner();

  if (auto libp2p = StartLibp2p(config_); !libp2p) {
    log().warning << "libp2p host start failed: " << libp2p.error().message
                  << " (direct P2P unavailable; relay messaging may still work)";
  }

  amp::PeerLinkManager* amp_links = nullptr;
  std::function<void()> amp_pump;
  std::function<void(std::function<void()>)> amp_worker;
  if (mesh_ && mesh_->Amp()) {
    amp_links = &mesh_->Amp()->Links();
    amp_pump = [this]() {
      if (mesh_) {
        mesh_->Tick();
      }
    };
    amp_worker = [](std::function<void()> task) {
      AppRuntime::PostWorkerNormal(std::move(task));
    };
  }

  p2p_ = std::make_unique<P2pMessagingService>(*store_, *contacts_, *identity_, relay_, *inbox_,
                                                signing_key_store_, *signing_resolver_, kem_key_store_, *kem_resolver_,
                                                *psk_store_, *group_roster_, group_invite_gate_.get(), amp_links,
                                                std::move(amp_pump), std::move(amp_worker));
  p2p_->SetProfileDataDir(data_dir_);
  p2p_->SetInitiationBillingStore(initiation_billing_.get());
  p2p_->SetPeerRouteSources(directory_shadows_.get(), directory_);
  WireAttachmentDownloads();
  group_membership_ = std::make_unique<GroupMembershipService>(*store_, *contacts_, *identity_, *group_roster_,
                                                               *group_invite_gate_, *p2p_);
  inbox_->SetGroupMembership(group_membership_.get());
  p2p_->SetGroupMembership(group_membership_.get());
  call_stack_->BuildSessions(MakeCallStackDeps());
  if (auto* calls = call_stack_->Calls()) {
    calls->SetInitiationBillingStore(initiation_billing_.get());
  }
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
  if (secrets_ == nullptr || !secrets_->IsUnlocked()) {
    return AppError::Pin(Err::Pin::Required, "Profile vault is locked");
  }
  if (auto identity = identity_->LoadOrCreate(); !identity) {
    return identity.error();
  } else {
    // Dogfood: seed LocalIdentity.initiation_floor from AppConfig when identity is still 0 (P001).
    if (identity->initiation_floor == 0 && config_.initiation_floor > 0) {
      LocalIdentity updated = *identity;
      updated.initiation_floor = config_.initiation_floor;
      if (auto saved = identity_->Update(updated); !saved) {
        return saved.error();
      }
    }
  }

  if (auto built = BuildMessagingStack(); !built) {
    return built.error();
  }

  messaging_ready_ = true;
  StartCoordinatorTimers();
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

void MessagingHub::BindSecrets(ProfileSecretsService& secrets) {
  secrets_ = &secrets;
}

PeerSigningKeyStore& MessagingHub::SigningKeys() {
  return signing_key_store_;
}

void MessagingHub::TickAmpMesh() {
  if (!messaging_ready_ || !mesh_) {
    return;
  }
  mesh_->Tick();
}

void MessagingHub::TickLibp2p() {
  if (!messaging_ready_) {
    return;
  }
  // Mesh UDP drain is on amp_mesh_pump_timer_ (~5ms). Policy stays on the 1s timer.
  if (p2p_) {
    p2p_->TickLibp2p();
  }
  SyncMobileEphemeralListen();
  SyncLanMdnsAdvertisement();
  TickReachabilityUx();
}

namespace {

/** Amp has no async reactor — product must Drive often enough for call-media / SFU / chat. */
constexpr auto kAmpMeshPumpInterval = std::chrono::milliseconds(5);
constexpr auto kHubPolicyTimerInterval = std::chrono::seconds(1);

} // namespace

void MessagingHub::StartCoordinatorTimers() {
  if (amp_mesh_pump_timer_id_ == 0 && mesh_) {
    amp_mesh_pump_timer_id_ =
        AppRuntime::ScheduleCoordinatorRepeating(kAmpMeshPumpInterval, [this]() { TickAmpMesh(); });
  }
  if (hub_policy_timer_id_ == 0) {
    hub_policy_timer_id_ = AppRuntime::ScheduleCoordinatorRepeating(kHubPolicyTimerInterval, [this]() {
      TickLibp2p();
    });
  }
  // Relay poll must not depend on ChatController::WireMessagingBindings (can skip if UI
  // wiring races unlock). Own the sync handler here as soon as messaging is ready.
  BackgroundSyncScheduler::Instance().SetSyncHandler([this](bool force) {
    const bool call_wake = BackgroundSyncScheduler::Instance().ConsumeCallWake();
    if (call_wake && on_call_wake_) {
      AppRuntime::PostUI([this]() {
        if (on_call_wake_) {
          on_call_wake_();
        }
      });
    }
    if (p2p_) {
      p2p_->SyncInboxFromWake(force);
    }
  });
  BackgroundSyncScheduler::Instance().RequestWakeSync();
}

void MessagingHub::StopCoordinatorTimers() {
  if (amp_mesh_pump_timer_id_ != 0) {
    AppRuntime::CancelCoordinatorTimer(amp_mesh_pump_timer_id_);
    amp_mesh_pump_timer_id_ = 0;
  }
  if (hub_policy_timer_id_ != 0) {
    AppRuntime::CancelCoordinatorTimer(hub_policy_timer_id_);
    hub_policy_timer_id_ = 0;
  }
  BackgroundSyncScheduler::Instance().SetSyncHandler(nullptr);
}

ReachabilitySnapshot MessagingHub::Reachability() const {
  return mesh_ ? mesh_->Reachability().Snapshot() : ReachabilitySnapshot{};
}

bool MessagingHub::IsHelpNetworkEnabled() const {
  return ResolveLibp2pRole(config_.libp2p) == Libp2pRole::Node;
}

void MessagingHub::SetOnReachabilityUpdated(std::function<void()> callback) {
  on_reachability_updated_ = std::move(callback);
}

void MessagingHub::SetOnPeerIconsChanged(std::function<void()> callback) {
  on_peer_icons_changed_ = std::move(callback);
}

void MessagingHub::NotifyPeerIconsChanged() {
  if (on_peer_icons_changed_) {
    on_peer_icons_changed_();
  }
}

std::string MessagingHub::ContactIconLocalPath(const Contact& contact) {
  const std::string key = ProfileIconCacheKeyForContact(contact);
  if (key.empty()) {
    return {};
  }
  return ProfileIconLocalPath(data_dir_, key);
}

std::string MessagingHub::IdentityIconLocalPath(const std::string& identity) {
  if (identity.empty() || !contacts_) {
    return {};
  }
  if (IsMessagingReady()) {
    if (auto local = Identity().Get()) {
      if (!local->account_id.empty() && local->account_id == identity) {
        const std::string self_path = ProfileIconLocalPath(data_dir_, "self");
        if (!self_path.empty()) {
          return self_path;
        }
      }
    }
  }
  if (auto found = contacts_->FindByIdentity(identity, ContactIdKind::Account)) {
    if (found->has_value()) {
      return ContactIconLocalPath(**found);
    }
  }
  if (auto found = contacts_->FindByIdentity(identity, ContactIdKind::RelayUser)) {
    if (found->has_value()) {
      return ContactIconLocalPath(**found);
    }
  }
  return ProfileIconLocalPath(data_dir_, SanitizeProfileIconCacheKey(identity));
}

void MessagingHub::EnsureDirectoryHitIconCached(const DirectoryHit& hit) {
  ScheduleDirectoryHitIconFetch(hit);
}

void MessagingHub::EnsureContactIconCached(const Contact& contact) {
  ScheduleContactIconFetch(contact);
}

void MessagingHub::ScheduleDirectoryHitIconFetch(const DirectoryHit& hit) {
  if (!hit.icon || hit.icon->url.empty()) {
    return;
  }
  const std::string key = ProfileIconCacheKeyForHit(hit);
  if (key.empty()) {
    return;
  }
  const ProfileIconRef icon = *hit.icon;
  AppRuntime::PostWorkerBackground([this, icon, key]() {
    if (!ProfileIconNeedsFetch(data_dir_, key, icon)) {
      return;
    }
    if (FetchProfileIcon(data_dir_, key, icon)) {
      AppRuntime::PostUI([this]() { NotifyPeerIconsChanged(); });
    }
  });
}

void MessagingHub::ScheduleContactIconFetch(const Contact& contact) {
  if (!contact.remote.icon || contact.remote.icon->url.empty()) {
    return;
  }
  const std::string key = ProfileIconCacheKeyForContact(contact);
  if (key.empty()) {
    return;
  }
  const ProfileIconRef icon = *contact.remote.icon;
  AppRuntime::PostWorkerBackground([this, icon, key]() {
    if (!ProfileIconNeedsFetch(data_dir_, key, icon)) {
      return;
    }
    if (FetchProfileIcon(data_dir_, key, icon)) {
      AppRuntime::PostUI([this]() { NotifyPeerIconsChanged(); });
    }
  });
}

void MessagingHub::RunReachabilityProbe(bool try_upnp) {
  if (!mesh_) {
    return;
  }
  mesh_->StartReachabilityProbe(try_upnp);
}

void MessagingHub::TryUpnpPortMapping() {
  if (!mesh_) {
    return;
  }
  // Amp listen uses UDP; mapping is attempted inside the reachability probe when try_upnp is set.
  mesh_->StartReachabilityProbe(true);
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
  view.account_id = identity->account_id;
  view.public_key_b64 = identity->account_signing_public_key_b64;
  FillRegistrationFields(view, *identity);
  view.profile_icon_path = ProfileIconLocalPath(data_dir_);
  view.profile_has_icon = !view.profile_icon_path.empty();
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

  std::vector<std::string> listen_addrs;
  if (mesh_ && !mesh_->AmpListenMultiaddr().empty()) {
    listen_addrs.push_back(mesh_->AmpListenMultiaddr());
  }
  auto applied = FinishAndPersistRegistration(Registration(), Identity(), identity->nickname, listen_addrs);
  if (!applied) {
    return applied.error();
  }
  return {};
}

Roe<void> MessagingHub::UploadProfileIconFromPath(const std::string& path) {
  if (!IsInitialized()) {
    return Error("Messaging hub not initialized");
  }
  if (!IsMessagingReady()) {
    return AppError::Pin(Err::Pin::Required, "Unlock profile PIN to change profile icon");
  }
  if (!blob_) {
    return Error("Blob client not configured");
  }
  auto encoded = PrepareProfileIconFromFile(path);
  if (!encoded) {
    return encoded.error();
  }
  PreparedProfileIcon prepared;
  prepared.bytes = std::move(encoded.value().bytes);
  prepared.content_type = encoded.value().content_type;
  prepared.kind = encoded.value().kind;
  prepared.file_extension = encoded.value().file_extension;
  auto uploaded = UploadPreparedProfileIcon(*blob_, Identity(), data_dir_, prepared);
  if (!uploaded) {
    return uploaded.error();
  }
  return {};
}

Roe<void> MessagingHub::ClearProfileIcon() {
  if (!IsInitialized()) {
    return Error("Messaging hub not initialized");
  }
  if (!IsMessagingReady()) {
    return AppError::Pin(Err::Pin::Required, "Unlock profile PIN to change profile icon");
  }
  if (!blob_) {
    return Error("Blob client not configured");
  }
  return ClearHostedProfileIcon(*blob_, Identity(), data_dir_);
}

namespace {

std::string ProtectedRelayBlobId(const std::string& profile_dir) {
  if (auto meta = LoadProfileIconCacheMeta(profile_dir); meta) {
    return meta->blob_id;
  }
  return {};
}

} // namespace

Roe<BlobQuotaRecoveryPlan> MessagingHub::PlanRelayQuotaRecovery() {
  if (!IsInitialized()) {
    return Error("Messaging hub not initialized");
  }
  if (!IsMessagingReady()) {
    return AppError::Pin(Err::Pin::Required, "Unlock profile PIN to manage relay blob storage");
  }
  if (!blob_) {
    return Error("Blob client not configured");
  }
  return PlanOldestRelayBlobDeletion(*blob_, Identity(), ProtectedRelayBlobId(data_dir_));
}

Roe<void> MessagingHub::FreeOldestRelayBlobSlot() {
  if (!IsInitialized()) {
    return Error("Messaging hub not initialized");
  }
  if (!IsMessagingReady()) {
    return AppError::Pin(Err::Pin::Required, "Unlock profile PIN to manage relay blob storage");
  }
  if (!blob_) {
    return Error("Blob client not configured");
  }
  return pbr::FreeOldestRelayBlobSlot(*blob_, Identity(), ProtectedRelayBlobId(data_dir_));
}

Roe<ThreadMessage> MessagingHub::SendAttachmentFromPath(const std::string& thread_id, const std::string& path) {
  if (!IsInitialized()) {
    return Error("Messaging hub not initialized");
  }
  if (!IsMessagingReady()) {
    return AppError::Pin(Err::Pin::Required, "Unlock profile PIN to send attachments");
  }
  if (!blob_) {
    return Error("Blob client not configured");
  }
  if (thread_id.empty()) {
    return Error("No active thread");
  }

  auto thread = Store().GetThread(thread_id);
  if (!thread) {
    return thread.error();
  }
  if (!*thread) {
    return Error("Thread not found");
  }
  const Thread& active = **thread;
  if (active.kind == ThreadKind::Ai) {
    return Error("Attachments are not supported in assistant threads");
  }
  if (active.kind != ThreadKind::Direct && active.kind != ThreadKind::Group) {
    return Error("Attachments are not supported in this thread");
  }

  ChatAttachmentUploadOptions upload_opts;
  upload_opts.peer_client = p2p_ ? p2p_->PeerBlobClient() : nullptr;
  upload_opts.contacts = contacts_.get();
  upload_opts.thread = &active;
  upload_opts.thread_id = thread_id;
  auto fields = UploadChatAttachmentFromFile(*blob_, Identity(), path, upload_opts);
  if (!fields) {
    return fields.error();
  }

  SendRelayOptions opts;
  opts.content_type = ChatContentType::Attachment;
  opts.payload_json = ChatPayloadCodec::AttachmentFieldsToJson(*fields);
  const std::string display = fields->filename.empty() ? "Attachment" : fields->filename;

  const ByteVector attachment_dek = Attachments().CopyDek();
  const ByteVector* attachment_dek_ptr = attachment_dek.empty() ? nullptr : &attachment_dek;

  if (active.kind == ThreadKind::Group) {
    auto sent = P2p().SendGroupMessage(thread_id, display, opts);
    if (sent) {
      (void)CopyAttachmentPlaintextFile(data_dir_, thread_id, *fields, path, attachment_dek_ptr, profile_id_);
      Attachments().MaybeBuildPoster(thread_id, *fields);
      if (attachment_downloads_) {
        attachment_downloads_->EnqueueFromMessage(thread_id, *sent);
      }
    }
    return sent;
  }
  auto sent = P2p().SendUserMessage(thread_id, display, opts);
  if (sent) {
    (void)CopyAttachmentPlaintextFile(data_dir_, thread_id, *fields, path, attachment_dek_ptr, profile_id_);
    Attachments().MaybeBuildPoster(thread_id, *fields);
    if (attachment_downloads_) {
      attachment_downloads_->EnqueueFromMessage(thread_id, *sent);
    }
  }
  return sent;
}

AttachmentDownloadService& MessagingHub::Attachments() {
  if (!attachment_downloads_) {
    attachment_downloads_ = std::make_unique<AttachmentDownloadService>();
    attachment_downloads_->SetProfileDataDir(data_dir_);
    attachment_downloads_->SetProfileId(profile_id_);
  }
  return *attachment_downloads_;
}

void MessagingHub::WireAttachmentDownloads() {
  if (!attachment_suppressions_) {
    attachment_suppressions_ = std::make_unique<AttachmentSuppressionStore>(data_dir_);
  } else {
    attachment_suppressions_->SetProfileDir(data_dir_);
  }
  if (!attachment_downloads_) {
    attachment_downloads_ = std::make_unique<AttachmentDownloadService>();
  }
  attachment_downloads_->SetProfileDataDir(data_dir_);
  attachment_downloads_->SetProfileId(profile_id_);
  attachment_downloads_->SetFetchDependencies(&Store(), contacts_.get(), identity_.get(),
                                                  p2p_ ? p2p_->PeerBlobClient() : nullptr);
  attachment_downloads_->SetSuppressionStore(attachment_suppressions_.get());
  if (auto prefs = UserPreferences::LoadProfile(data_dir_); prefs) {
    attachment_downloads_->SetDownloadPolicy(AttachmentDownloadPolicyFromString(prefs->attachment_download_policy));
  }
  attachment_downloads_->SetOnChanged([this]() {
    if (p2p_) {
      p2p_->NotifyMessagesChanged();
    }
  });
  if (inbox_) {
    inbox_->SetProfileDataDir(data_dir_);
    inbox_->SetAttachmentDownloads(attachment_downloads_.get());
  }
  if (p2p_) {
    p2p_->SetAttachmentDownloads(attachment_downloads_.get());
    if (auto* peer_blob = p2p_->PeerBlobService()) {
      peer_blob->SetProfileId(profile_id_);
      if (secrets_ != nullptr) {
        secrets_->RegisterDekConsumer(peer_blob);
        if (secrets_->IsUnlocked()) {
          (void)secrets_->RedistributeUnlockedDek();
        }
      }
    }
  }
  if (secrets_ != nullptr) {
    secrets_->RegisterDekConsumer(attachment_downloads_.get());
    if (secrets_->IsUnlocked()) {
      (void)secrets_->RedistributeUnlockedDek();
    }
  }
}

void MessagingHub::RequestAttachmentDownload(const std::string& thread_id, const std::string& message_id) {
  if (!IsInitialized() || thread_id.empty() || message_id.empty()) {
    return;
  }
  Attachments().RequestDownload(thread_id, message_id, Store());
}

void MessagingHub::DrainPendingAttachmentMedia() {
  if (!IsInitialized()) {
    return;
  }
  Attachments().DrainPendingMediaBacklog(Store());
}

Roe<void> MessagingHub::ClearDownloadedAttachments() {
  if (!IsInitialized()) {
    return Error("Messaging hub not initialized");
  }
  auto cleared = Attachments().ClearAllDownloadedMedia(Store());
  if (cleared && p2p_) {
    p2p_->NotifyMessagesChanged();
  }
  return cleared;
}

Roe<void> MessagingHub::SendChargeRequired(const std::string& peer_identity,
                                           const std::optional<int64_t> floor_minor) {
  if (!IsInitialized() || !IsMessagingReady() || !p2p_) {
    return Error("Messaging not ready");
  }
  return p2p_->SendChargeRequired(peer_identity, floor_minor);
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
    base_url = BriefLlmBaseUrl();
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
    auto json = TryParseObject(response->body);
    std::string detail = "Brief API key rotate failed (HTTP " + std::to_string(response->status_code) + ")";
    if (json) {
      if (auto err_str = json->getString("error")) {
        detail = *err_str;
      } else if (const Object* err_obj = json->getObject("error")) {
        if (auto message = err_obj->getString("message")) {
          detail = *message;
        }
      }
    }
    return AppError::Network(Err::Network::HttpError, detail);
  }

  auto root = TryParseObject(response->body);
  auto new_key_opt = root ? root->getString("llm_api_key") : std::nullopt;
  if (!new_key_opt) {
    return AppError::Auth(Err::Auth::Generic, "Brief API key rotate response missing llm_api_key")
        .WithUser("Couldn't update Brief API key — try Renew registration in Me → Profile.");
  }
  const std::string new_key = *new_key_opt;
  if (new_key.empty()) {
    return AppError::Auth(Err::Auth::Generic, "Brief API key rotate returned empty key")
        .WithUser("Couldn't update Brief API key — try Renew registration in Me → Profile.");
  }

  LocalIdentity updated = *identity;
  updated.brief_llm_api_key = new_key;
  updated.brief_llm_guest_api_key.clear();
  if (auto saved = Identity().Update(updated); !saved) {
    return saved.error();
  }
  return {};
}

void MessagingHub::TickReachabilityUx() {
  if (!config_.libp2p.node_enabled || Platform::IsMobile()) {
    return;
  }
  const ReachabilitySnapshot snap = Reachability();
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
  if (!messaging_ready_ || !mesh_) {
    return;
  }
  if (session_store_ && session_store_->IsInitialized()) {
    config_.libp2p = session_store_->Snapshot().config.libp2p;
  }
  const Libp2pRole role = ResolveLibp2pRole(config_.libp2p);
  // Amp L4 inbound hosting is gated via SetServeInbound (no TCP CircuitRelay/MediaRelay).
  if (mesh_->AmpCircuitTunnel()) {
    mesh_->AmpCircuitTunnel()->SetServeInbound(role == Libp2pRole::Node &&
                                               config_.libp2p.capabilities.circuit_relay);
  }
  call_stack_->ResetRelayClients();
  if (mesh_->AmpMediaRelayCoord()) {
    mesh_->AmpMediaRelayCoord()->SetServeInbound(role == Libp2pRole::Node &&
                                                 config_.libp2p.capabilities.media_relay);
  }
  ApplyMeshAdmissionPolicies();
  call_stack_->WireMediaRelayDeps();
  SyncMobileEphemeralListen();
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
  if (attachment_downloads_) {
    attachment_downloads_->SetDownloadPolicy(prefs.attachment_download_policy);
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
  return {.group_invite_policy = GroupInvitePolicyFromString(prefs.group_invite_policy),
          .attachment_download_policy = AttachmentDownloadPolicyFromString(prefs.attachment_download_policy)};
}

MessagingHub::NotificationPrefs MessagingHub::ProjectNotifications(const ProfilePreferences& prefs) {
  return {.show_notifications = prefs.show_notifications};
}

Roe<CircuitRelayBridgeResult> MessagingHub::RequestCircuitBridgePreferred(const std::string& target_peer_id,
                                                                          const std::string& target_multiaddr,
                                                                          int timeout_ms) {
  if (target_peer_id.empty() && target_multiaddr.empty()) {
    return Error("missing circuit bridge target");
  }
  CircuitTunnelCoordinator* amp_circuit = mesh_ ? mesh_->AmpCircuitTunnel() : nullptr;
  if (!amp_circuit || !amp_circuit->IsStarted() || !mesh_ || !mesh_->Amp()) {
    return Error("Amp circuit-relay required");
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

  CircuitBridgeTarget target;
  target.target_peer_id = target_peer_id;
  target.target_multiaddr = target_multiaddr;

  CircuitRelayBridgeResult last;
  last.error = "all hops failed";

  amp::PeerLinkManager& links = mesh_->Amp()->Links();
  for (const MeshHopCandidate& hop : hops) {
    const std::string key = hop.peer_id;
    if (!target.target_peer_id.empty() && key == target.target_peer_id) {
      continue;
    }
    if (!hop.multiaddr.empty()) {
      (void)links.RegisterEndpoint(key, hop.multiaddr);
    } else if (auto ma = links.PreferredMultiaddr(key)) {
      (void)links.RegisterEndpoint(key, *ma);
    }
    if (!links.GetLinkSnapshot(key).has_endpoint) {
      last.error = "hop not dialable: " + key;
      continue;
    }
    SettledWait<CircuitTunnelBridgeResult> wait;
    (void)amp_circuit->StartBridge(key, target, {}, {},
                                   [wait](Roe<CircuitTunnelBridgeResult> result) {
                                     wait.Finish(std::move(result));
                                   },
                                   timeout_ms);
    auto bridged =
        wait.Wait(std::chrono::milliseconds(timeout_ms + 500), Error("amp circuit bridge timed out"));
    if (!bridged) {
      last.error = bridged.error().message;
      continue;
    }
    if (!bridged->ok) {
      last.error = bridged->error.empty() ? "amp circuit bridge failed" : bridged->error;
      continue;
    }
    CircuitRelayBridgeResult ok;
    ok.ok = true;
    ok.resolved_multiaddr = bridged->resolved_multiaddr;
    return ok;
  }
  return last;
}


void MessagingHub::SuspendLibp2pColdPeers() {
  SyncMobileEphemeralListen();
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
  if (auto* calls = call_stack_->Calls()) {
    calls->ClearMediaCallbacks();
  }

  router_.reset();
  store_->Flush();
  contacts_->Flush();
  StopCoordinatorTimers();
  on_call_wake_ = nullptr;
  on_messaging_ready_ = nullptr;
  if (messaging_ready_) {
    identity_->Flush();
  }
  actions_.reset();
  // Stop libp2p / Connect workers before dropping session façade (Leave may still be dialing).
  StopLibp2p();
  // Drop the call session manager before P2P — CSM holds a P2pMessagingService& reference.
  call_stack_->ResetSessions();
  if (secrets_ != nullptr) {
    if (attachment_downloads_) {
      secrets_->UnregisterDekConsumer(attachment_downloads_.get());
    }
    if (p2p_) {
      if (auto* peer_blob = p2p_->PeerBlobService()) {
        secrets_->UnregisterDekConsumer(peer_blob);
      }
    }
  }
  // Destroy P2P before groups — P2P held a non-owning Groups pointer.
  p2p_.reset();
  group_membership_.reset();
  group_invite_gate_.reset();
  group_roster_.reset();
  signing_resolver_.reset();
  kem_resolver_.reset();
  if (secrets_ != nullptr) {
    if (identity_) {
      secrets_->UnregisterDekConsumer(identity_.get());
    }
    if (psk_store_) {
      secrets_->UnregisterDekConsumer(psk_store_.get());
    }
    if (store_) {
      secrets_->UnregisterDekConsumer(static_cast<SqliteThreadStore*>(store_.get()));
    }
    if (call_stack_->MediaKeys()) {
      secrets_->UnregisterDekConsumer(call_stack_->MediaKeys());
    }
  }
  // Reset media engine / key store / session store after DEK unregister above.
  call_stack_->Shutdown();
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
  return call_stack_->Calls();
}

CallLifecycle* MessagingHub::Lifecycle() {
  return call_stack_->Lifecycle();
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

IPskSessionStore* MessagingHub::PskStore() {
  return psk_store_.get();
}

ProfileSecretsService* MessagingHub::Secrets() {
  return secrets_;
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

IBlobClient& MessagingHub::Blob() {
  return *blob_;
}

IPushDeviceClient* MessagingHub::PushDevices() {
  return push_devices_;
}

IClientCompatClient* MessagingHub::ClientCompat() {
  return client_compat_;
}

} // namespace pbr
