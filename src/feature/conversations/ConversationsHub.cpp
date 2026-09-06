#include "feature/conversations/ConversationsHub.h"
#include "domain/messaging/PaymentPromiseLifecycle.h"
#include "domain/messaging/PaymentPromiseAvoid.h"
#include "domain/messaging/PaymentPromiseWireCodec.h"
#include "common/ValueJson.h"

#include "domain/people/ContactReachability.h"
#include "feature/conversations/GroupInviteGate.h"
#include "feature/conversations/AttachmentFetchWorkflow.h"
#include "feature/conversations/GroupMembershipWorkflow.h"
#include "feature/conversations/RelayDirectoryKemKeyResolver.h"
#include "feature/conversations/RelayDirectorySigningKeyResolver.h"
#include "domain/messaging/SqlitePskSessionStore.h"

#include "feature/conversations/PushDeviceCoordinator.h"
#include "foundation/crypto/ProfileSecretsEngine.h"
#include "foundation/data/LlmPreset.h"
#include "foundation/platform/DeploymentProfile.h"
#include "foundation/error/AppError.h"
#include "foundation/data/MeshRole.h"
#include "foundation/data/SessionStore.h"
#include "foundation/data/UserPreferences.h"
#include "domain/messaging/AttachmentCache.h"
#include "domain/people/DirectChatTargetFromContact.h"
#include "domain/messaging/ChatPayloadCodec.h"
#include "domain/messaging/GroupTypes.h"
#include "domain/messaging/SendRelayOptions.h"
#include "feature/conversations/AttachmentClientUtil.h"
#include "domain/net/BlobQuotaUtil.h"
#include "domain/net/HttpClient.h"
#include "feature/conversations/ProfileIconClientUtil.h"
#include "feature/conversations/ProfileIconFetchUtil.h"
#include "feature/conversations/RegistrationClientUtil.h"
#include "domain/people/ProfileIconCache.h"
#include "foundation/platform/ProfileIconImagePrep.h"
#include "domain/people/ContactIdentity.h"
#include "domain/people/ContactTypes.h"
#include "foundation/runtime/AppLifecycle.h"
#include "foundation/runtime/BackgroundSyncScheduler.h"
#include "foundation/runtime/AppRuntime.h"
#include "foundation/platform/NetworkConnectivity.h"
#include "foundation/platform/Platform.h"
#include "foundation/data/PlatformDefaults.h"
#include "domain/mesh/l4/circuit/CircuitBridgeTarget.h"
#include "domain/mesh/l4/circuit/CircuitRelayTypes.h"
#include "domain/mesh/l4/media_relay/MediaRelayTypes.h"
#include "domain/mesh/l4/circuit/CircuitTunnelCoordinator.h"
#include "domain/mesh/reachability/LanMdnsDiscovery.h"
#include "common/SettledWait.h"
#include "domain/people/MeshHopPolicy.h"
#include "domain/mesh/dht/DhtRecordCodec.h"
#include "domain/mesh/discovery/AmpDirectoryProtocol.h"
#include "domain/mesh/discovery/NameDirectory.h"
#include "domain/mesh/host/MeshPorts.h"
#include "domain/mesh/reachability/NatTraversal.h"
#include "domain/mesh/reachability/Reachability.h"
#include "common/StartupTiming.h"
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

ConversationsHub::ConversationsHub() {
  redirectLogger("ConversationsHub");
  // Always own an (empty) CallStack so CallUiBackend can bind CallStackRef() at construction,
  // even before Initialize builds the call session/media objects.
  call_stack_ = std::make_unique<CallStack>();
}

CallStackDeps ConversationsHub::MakeCallStackDeps() {
  CallStackDeps deps;
  deps.store = store_.get();
  deps.contacts = contacts_.get();
  deps.identity = identity_.get();
  deps.psk = psk_store_.get();
  if (mesh_messaging_) {
    deps.delivery.send_user_message = [this](const std::string& thread_id, const std::string& text,
                                             const SendRelayOptions& options) {
      return mesh_messaging_->SendUserMessage(thread_id, text, options);
    };
    deps.delivery.sync_inbox_from_wake = [this](bool force) { mesh_messaging_->SyncInboxFromWake(force); };
    deps.delivery.register_peer_direct_endpoint = [this](const std::string& identity,
                                                         const std::string& multiaddr) {
      mesh_messaging_->RegisterPeerDirectEndpoint(identity, multiaddr);
    };
    deps.bind_call_control = [this](CallControlInboundPorts ports) {
      mesh_messaging_->BindCallControlInbound(std::move(ports));
    };
  }
  deps.mesh = [this]() { return mesh_.get(); };
  deps.config = [this]() -> const AppConfig& { return config_; };
  deps.list_directory_nodes = [this]() {
    return mesh_directory_cache_ ? mesh_directory_cache_->Snapshot() : std::vector<MeshDirectoryNode>{};
  };
  deps.list_dht_nodes = [this]() {
    if (!mesh_ || !mesh_->AmpDht()) {
      return std::vector<MeshDirectoryNode>{};
    }
    return MeshDirectoryNodesFromDhtRecords(mesh_->AmpDht()->SnapshotRecords());
  };
  deps.seed_dial_ok = [this]() {
    if (!mesh_) {
      return true;
    }
    return mesh_->Reachability().Snapshot().signals.seed_dial_ok;
  };
  deps.prefetch_peer_reachability = [this](const std::string& identity) {
    PrefetchPeerReachability(identity);
  };
  deps.sync_mobile_ephemeral_listen = [this]() { SyncMobileEphemeralListen(); };
  deps.note_lan_mdns_peer_id = [this](const std::string& peer_id) {
    lan_mdns_contact_peer_ids_.insert(peer_id);
  };
  return deps;
}

ConversationsHub::~ConversationsHub() {
  Shutdown();
}

void ConversationsHub::WireRelayAuthSigner() {
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

void ConversationsHub::UpdateOrgBackendClients(const AppConfig& config) {
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

  if (!directory_url.empty() || !config.directory.providers.empty()) {
    DirectoryConfig dir_cfg = config.directory;
    if (dir_cfg.base_url.empty()) {
      dir_cfg.base_url = directory_url;
    }
    NormalizeDirectoryConfig(dir_cfg);
    const std::vector<ServiceEndpointConfig> providers = EffectiveDirectoryProviders(dir_cfg);
    std::vector<std::unique_ptr<IDirectoryClient>> backends;
    backends.reserve(providers.size());
    std::string fingerprint;
    for (const ServiceEndpointConfig& provider : providers) {
      const std::string transport = provider.transport.empty() ? "http" : provider.transport;
      if (transport != "http") {
        // Amp backends need MeshHost / PeerLinkManager — wired in ConfigureAmpDirectoryProtocol +
        // MeshDirectoryCache Amp-first fetcher (N029 nd4). Factory skips the same way.
        continue;
      }
      fingerprint.append(provider.base_url).push_back('|');
      backends.push_back(std::make_unique<HttpDirectoryClient>(provider.base_url));
    }
    if (backends.empty()) {
      directory_owned_.reset();
      http_directory_url_.clear();
      directory_ = nullptr;
      log().warning << "directory providers empty; directory client disabled";
    } else if (!directory_owned_ || http_directory_url_ != fingerprint) {
      http_directory_url_ = std::move(fingerprint);
      if (backends.size() == 1) {
        directory_owned_ = std::move(backends.front());
      } else {
        directory_owned_ = std::make_unique<FailoverDirectoryClient>(std::move(backends));
      }
      directory_ = directory_owned_.get();
    }
  } else {
    directory_owned_.reset();
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

void ConversationsHub::InstallOrgBackendClients(const AppConfig& config) {
  UpdateOrgBackendClients(config);
}

Roe<void> ConversationsHub::StartMesh(const AppConfig& config) {
  StartupPhase phase("ConversationsHub::StartMesh");
  if (shutdown_requested_.load(std::memory_order_acquire)) {
    return Error("shutdown in progress");
  }
  StopMesh();
  mesh_last_error_.clear();

  MeshConfig product_mesh_cfg = config.mesh;
  NormalizeMeshConfig(product_mesh_cfg);
  const MeshRole role = ResolveMeshRole(product_mesh_cfg);

  MeshHostConfig mesh_cfg;
  if (auto priv = identity_->GetDeviceMlDsaPrivateKey()) {
    mesh_cfg.host.device_ml_dsa_private_key = *priv;
  }
  if (auto pub = identity_->GetDeviceMlDsaPublicKey()) {
    mesh_cfg.host.device_ml_dsa_public_key = *pub;
  }
  mesh_cfg.host_circuit_relay = role == MeshRole::Node && config_.mesh.capabilities.circuit_relay;
  mesh_cfg.host_media_relay = role == MeshRole::Node && config_.mesh.capabilities.media_relay;
  mesh_cfg.host_dht = role == MeshRole::Node && config_.mesh.capabilities.dht;
  // Org seed / desktop Node: serve Amp directory twin (N029 nd4).
  mesh_cfg.host_directory = role == MeshRole::Node;
  mesh_cfg.media_relay_budget = config_.mesh.media_relay_budget;
  mesh_cfg.media_relay_pricing = config_.mesh.pricing.media_relay;
  // Skip blocking UPnP/seed probe when the app is already quitting (sync probe when no post_worker).
  mesh_cfg.start_reachability_probe =
      role == MeshRole::Node && !shutdown_requested_.load(std::memory_order_acquire);
  if (role == MeshRole::Node) {
    mesh_cfg.try_upnp_first = !upnp_auto_tried_;
    upnp_auto_tried_ = true;
  }
  mesh_cfg.on_reachability_updated = [this]() {
    if (shutdown_requested_.load(std::memory_order_acquire)) {
      return;
    }
    ApplyMeshAdmissionPolicies();
    PublishNodeAdvertisedAddrs();
    RegisterContactEndpoints();
    if (on_reachability_updated_) {
      on_reachability_updated_();
    }
  };
  mesh_cfg.mesh_enabled = product_mesh_cfg.mesh_enabled && mesh_cfg.host.device_ml_dsa_private_key &&
                          mesh_cfg.host.device_ml_dsa_public_key;
  mesh_cfg.amp_udp_port =
      product_mesh_cfg.amp_udp_port <= 0 ? 0 : static_cast<uint16_t>(product_mesh_cfg.amp_udp_port);
  mesh_cfg.bootstrap_peers = product_mesh_cfg.bootstrap_peers;

  mesh_ = std::make_unique<MeshHost>();
  auto started = mesh_->Start(mesh_cfg);
  if (!started) {
    mesh_last_error_ = mesh_->LastError().empty() ? started.error().message : mesh_->LastError();
    mesh_.reset();
    return started.error();
  }
  if (shutdown_requested_.load(std::memory_order_acquire)) {
    mesh_->Stop();
    mesh_.reset();
    return Error("shutdown in progress");
  }
  if (mesh_cfg.mesh_enabled) {
    log().info << "amp stack listen=" << mesh_->AmpListenMultiaddr();
  } else {
    log().info << "mesh disabled (mesh_enabled=false); peer mesh underlay off";
  }
  StartMeshServices();
  ConfigureAmpDhtProtocol();
  ConfigureAmpDirectoryProtocol();
  if (mesh_directory_cache_) {
    mesh_directory_cache_->RequestRefresh();
  }
  if (shutdown_requested_.load(std::memory_order_acquire)) {
    StopMesh();
    return Error("shutdown in progress");
  }
  return {};
}

void ConversationsHub::StartMeshServices() {
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

void ConversationsHub::PublishNodeAdvertisedAddrs() {
  // A017: Identify/Host advertisement retired. Amp peers learn via mDNS / pasted ADP MAs.
}



void ConversationsHub::PublishMobileCallScopedAddrs() {
  // A017: TCP ephemeral Identify publish retired; Amp listen is always advertised via mDNS.
}



void ConversationsHub::SyncMobileEphemeralListen() {
  if (!Platform::IsMobile() || !messaging_ready_) {
    return;
  }
  // D10/A017: Amp UDP accept is always on — TCP ephemeral listen retired. Refresh mDNS for LAN.
  if (mesh_ && mesh_->Amp()) {
    SyncLanMdnsAdvertisement();
  }
}



void ConversationsHub::SyncLanMdnsAdvertisement() {
  if (!lan_mdns_ || !lan_mdns_->IsRunning()) {
    return;
  }

  const bool amp_up = mesh_ && mesh_->Amp() && !mesh_->AmpListenMultiaddr().empty();
  if (!amp_up) {
    return;
  }

  const MeshRole role = ResolveMeshRole(config_.mesh);
  const bool node_listen = role == MeshRole::Node;
  const bool ephemeral = Platform::IsMobile() && call_stack_ && call_stack_->WantEphemeralListen();
  if (!node_listen && !ephemeral) {
    lan_mdns_->SetAdvertisement({}, 0, {});
    return;
  }

  const std::string peer_id = mesh_->Amp()->LocalPeerId();
  int amp_udp = 0;
  if (auto port = UdpPortFromAdpMultiaddr(mesh_->AmpListenMultiaddr())) {
    amp_udp = static_cast<int>(*port);
  }
  if (peer_id.empty() || amp_udp <= 0) {
    lan_mdns_->SetAdvertisement({}, 0, {});
    return;
  }

  // Empty host_ips → LanMdnsDiscovery enumerates LAN IFs (Amp typically binds 0.0.0.0).
  lan_mdns_->SetAdvertisement(peer_id, amp_udp, {});
}


void ConversationsHub::OnLanMdnsPeerDiscovered(const LanMdnsDiscoveredPeer& peer) {
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
    if (!(mesh_messaging_ && (adp || ma))) {
      return;
    }
    log().info << "LAN mDNS discovered peer=" << peer.peer_id_base58
               << " ma=" << (ma ? *ma : "") << " adp=" << (adp ? *adp : "");
    if (mesh_messaging_) {
      if (ma) {
        mesh_messaging_->RegisterPeerDirectEndpoint(peer.peer_id_base58, *ma);
      }
      if (adp) {
        log().info << "LAN mDNS amp dial peer=" << peer.peer_id_base58 << " ma=" << *adp;
        mesh_messaging_->RegisterPeerDirectEndpoint(peer.peer_id_base58, *adp);
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
          if (mesh_messaging_) {
            if (ma) {
              mesh_messaging_->RegisterPeerDirectEndpoint(target.peer_identity_value, *ma);
            }
            if (auto adp2 = LanMdnsDiscovery::BuildAdpMultiaddr(peer)) {
              mesh_messaging_->RegisterPeerDirectEndpoint(target.peer_identity_value, *adp2);
            }
          }
          log().info << "LAN mDNS dial alias peer=" << peer.peer_id_base58
                        << " dial_key=" << target.peer_identity_value;
        }
      }
    }
  });
}


void ConversationsHub::ApplyMeshAdmissionPolicies() {
  const bool prefer = config_.mesh.prefer_contacts_for_routing;
  const bool node = ResolveMeshRole(config_.mesh) == MeshRole::Node;
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


void ConversationsHub::StopMesh() {
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
  // Amp chat/announce/broadcast transports hold PeerLinks& — detach before MeshHost::Stop
  // destroys Amp (otherwise ~AmpBroadcastTransport::Stop UAFs).
  if (mesh_messaging_) {
    mesh_messaging_->DetachAmpTransports();
  }
  // MeshHost::Stop tears down media_relay, circuit, dial-back, and runtime (in that order).
  // Keep bridge + dial registry alive until the mesh host joins its workers — inbound
  // CallMediaKey wait and OpenStream completions may still touch them.
  if (mesh_) {
    mesh_->Stop();
    mesh_.reset();
  }
  call_stack_->FinishMeshStop();
}

void ConversationsHub::AbortCallMediaForShutdown() {
  call_stack_->AbortCallMediaForShutdown();
}

void ConversationsHub::RequestShutdown() {
  shutdown_requested_.store(true, std::memory_order_release);
  // Unblock circuit / dial waits inside an in-flight StartMesh / EnsureMessagingReady.
  AbortCallMediaForShutdown();
  if (mesh_) {
    mesh_->AbortInflightCircuitRequests();
  }
}

void ConversationsHub::DiscardMessagingBringUp() {
  StopCoordinatorTimers();
  if (router_) {
    router_->SetOnLocalAction(nullptr);
    router_->SetSharedAiConfirmCallback(nullptr);
    router_.reset();
  }
  if (actions_) {
    actions_->SetOnActionMessage(nullptr);
    actions_.reset();
  }
  if (mesh_messaging_) {
    mesh_messaging_->SetOnMessagesChanged(nullptr);
    mesh_messaging_->SetOnDeliveryNotice(nullptr);
    mesh_messaging_->SetOnBackgroundUnread(nullptr);
    mesh_messaging_->SetGroupMembership(nullptr);
    mesh_messaging_->SetAttachmentDownloads(nullptr);
  }
  if (inbox_) {
    inbox_->SetGroupMembership(nullptr);
    inbox_->SetAttachmentDownloads(nullptr);
  }
  group_membership_.reset();
  // Unregister DEK consumers before StopMesh DetachAmpTransports destroys peer_blob.
  if (secrets_ != nullptr) {
    if (attachment_downloads_) {
      secrets_->UnregisterDekConsumer(attachment_downloads_.get());
    }
    if (mesh_messaging_) {
      if (auto* peer_blob = mesh_messaging_->PeerBlobService()) {
        secrets_->UnregisterDekConsumer(peer_blob);
      }
    }
  }
  StopMesh();
  call_stack_->ResetSessions();
  mesh_messaging_.reset();
  messaging_ready_ = false;
  mesh_ready_ = false;
  mesh_bringup_scheduled_.store(false, std::memory_order_release);
}

void ConversationsHub::RegisterContactEndpoints() {
  if (!mesh_messaging_ || !contacts_) {
    return;
  }
  auto listed = contacts_->List();
  if (!listed) {
    return;
  }
  lan_mdns_contact_peer_ids_.clear();
  for (const Contact& contact : *listed) {
    mesh_messaging_->RegisterContactDirectEndpoints(contact);
    const DirectChatTarget target = DirectChatTargetFromContact(contact, ThreadChannel::E2ePublic);
    if (target.peer_identity_value.empty()) {
      continue;
    }
    for (const std::string& ma : contact.multiaddrs) {
      mesh_messaging_->RegisterPeerDirectEndpoint(target.peer_identity_value, ma);
    }
    const std::vector<std::string> peer_ids = PeerIdsFromContact(contact);
    for (const std::string& peer_id : peer_ids) {
      lan_mdns_contact_peer_ids_.insert(peer_id);
    }
  }
  ApplyMeshAdmissionPolicies();
}

void ConversationsHub::RegisterMeshDirectoryEndpoints() {
  if (!mesh_messaging_ || !mesh_directory_cache_) {
    return;
  }
  for (const MeshDirectoryNode& node : mesh_directory_cache_->Snapshot()) {
    for (const std::string& ma : node.multiaddrs) {
      if (ma.empty()) {
        continue;
      }
      mesh_messaging_->RegisterPeerDirectEndpoint(node.peer_id, ma);
    }
  }
}

namespace {

std::vector<std::string> CollectDhtQueryPeerKeys(const std::vector<std::string>& bootstrap_peers,
                                                 const std::vector<MeshDirectoryNode>& directory_nodes) {
  std::vector<std::string> keys;
  std::unordered_set<std::string> seen;
  for (const std::string& ma : bootstrap_peers) {
    const std::string peer_id = PeerIdFromMultiaddr(ma);
    if (peer_id.empty() || !seen.insert(peer_id).second) {
      continue;
    }
    keys.push_back(peer_id);
  }
  for (const MeshDirectoryNode& node : directory_nodes) {
    if (node.peer_id.empty() || !seen.insert(node.peer_id).second) {
      continue;
    }
    keys.push_back(node.peer_id);
  }
  return keys;
}

} // namespace

void ConversationsHub::RegisterDhtBootstrapEndpoints() {
  if (!mesh_messaging_) {
    return;
  }
  MeshConfig mesh_cfg = config_.mesh;
  NormalizeMeshConfig(mesh_cfg);
  for (const std::string& ma : mesh_cfg.bootstrap_peers) {
    const std::string peer_id = PeerIdFromMultiaddr(ma);
    if (peer_id.empty() || ma.empty()) {
      continue;
    }
    mesh_messaging_->RegisterPeerDirectEndpoint(peer_id, ma);
  }
  RegisterMeshDirectoryEndpoints();
}

void ConversationsHub::ApplyDhtFindPeerResult(const std::string& peer_id, const PeerRoutingRecord& record) {
  if (!mesh_messaging_ || peer_id.empty()) {
    return;
  }
  for (const std::string& ma : record.multiaddrs) {
    if (ma.empty()) {
      continue;
    }
    mesh_messaging_->RegisterPeerDirectEndpoint(peer_id, ma);
  }
}

void ConversationsHub::ConfigureAmpDhtProtocol() {
  if (!mesh_ || !mesh_->Amp() || !mesh_->AmpDht() || !identity_) {
    return;
  }
  const MeshRole role = ResolveMeshRole(config_.mesh);
  const bool participate = role == MeshRole::Node && config_.mesh.capabilities.dht;

  RegisterDhtBootstrapEndpoints();

  std::vector<MeshDirectoryNode> directory_nodes;
  if (mesh_directory_cache_) {
    directory_nodes = mesh_directory_cache_->Snapshot();
  }
  MeshConfig mesh_cfg = config_.mesh;
  NormalizeMeshConfig(mesh_cfg);

  AmpDhtProtocolConfig cfg;
  cfg.local_peer_id = mesh_->Amp()->LocalPeerId();
  if (!mesh_->AmpListenMultiaddr().empty()) {
    cfg.listen_multiaddrs = {mesh_->AmpListenMultiaddr()};
  }
  if (auto priv = identity_->GetDeviceMlDsaPrivateKey()) {
    cfg.device_signing_secret = *priv;
  }
  if (auto pub = identity_->GetDeviceMlDsaPublicKey()) {
    cfg.device_signing_public = *pub;
  }
  cfg.tunables = config_.mesh.dht;
  cfg.query_peer_keys = CollectDhtQueryPeerKeys(mesh_cfg.bootstrap_peers, directory_nodes);
  cfg.participate = participate;
  cfg.publish_circuit_relay = participate && config_.mesh.capabilities.circuit_relay;
  cfg.publish_media_relay = participate && config_.mesh.capabilities.media_relay;
  mesh_->ConfigureAmpDht(std::move(cfg));
  mesh_->RefreshAmpDhtHosting(participate);
}

namespace {

std::string AmpDirectoryPeerKeyFromBaseUrl(const std::string& base_url) {
  if (base_url.empty()) {
    return {};
  }
  if (base_url.find('/') != std::string::npos) {
    return PeerIdFromMultiaddr(base_url);
  }
  return base_url;
}

MeshNodeHit BuildLocalMeshNodeHit(IdentityStore& identity, MeshHost& mesh, const MeshConfig& mesh_cfg) {
  MeshNodeHit hit;
  hit.entity_kind = "mesh_node";
  if (auto loaded = identity.Get()) {
    hit.relay_user_id = loaded->relay_user_id.empty() ? loaded->peer_id : loaded->relay_user_id;
    if (!loaded->account_id.empty()) {
      hit.account_id = loaded->account_id;
    }
    if (!loaded->nickname.empty()) {
      hit.nickname = loaded->nickname;
    }
    if (!loaded->public_key_b64.empty()) {
      hit.signing_public_key_b64 = loaded->public_key_b64;
    }
    if (!loaded->kem_public_key_b64.empty()) {
      hit.kem_public_key_b64 = loaded->kem_public_key_b64;
    }
  }
  if (hit.relay_user_id.empty() && mesh.Amp()) {
    hit.relay_user_id = mesh.Amp()->LocalPeerId();
  }
  hit.capabilities.circuit_relay = mesh_cfg.capabilities.circuit_relay;
  hit.capabilities.media_relay = mesh_cfg.capabilities.media_relay;
  hit.capabilities.dht = mesh_cfg.capabilities.dht;
  hit.capabilities.ledger_gateway = mesh_cfg.capabilities.ledger_gateway;
  DirectoryEndpoint ep;
  if (mesh.Amp()) {
    ep.peer_id = mesh.Amp()->LocalPeerId();
  }
  if (!mesh.AmpListenMultiaddr().empty()) {
    ep.multiaddrs.push_back(mesh.AmpListenMultiaddr());
  }
  for (const std::string& ma : mesh_cfg.advertise_multiaddrs) {
    if (!ma.empty() &&
        std::find(ep.multiaddrs.begin(), ep.multiaddrs.end(), ma) == ep.multiaddrs.end()) {
      ep.multiaddrs.push_back(ma);
    }
  }
  if (!ep.peer_id.empty()) {
    hit.endpoints.push_back(std::move(ep));
  }
  return hit;
}

} // namespace

void ConversationsHub::ConfigureAmpDirectoryProtocol() {
  if (!mesh_ || !mesh_->Amp() || !mesh_->AmpDirectory() || !identity_) {
    return;
  }
  const MeshRole role = ResolveMeshRole(config_.mesh);
  const bool host_directory = role == MeshRole::Node;

  RegisterDhtBootstrapEndpoints();

  MeshConfig mesh_cfg = config_.mesh;
  NormalizeMeshConfig(mesh_cfg);

  std::vector<std::string> query_keys;
  std::unordered_set<std::string> seen;
  DirectoryConfig dir_cfg = config_.directory;
  NormalizeDirectoryConfig(dir_cfg);
  for (const ServiceEndpointConfig& provider : EffectiveDirectoryProviders(dir_cfg)) {
    const std::string transport = provider.transport.empty() ? "http" : provider.transport;
    if (transport != "amp") {
      continue;
    }
    const std::string key = AmpDirectoryPeerKeyFromBaseUrl(provider.base_url);
    if (key.empty() || !seen.insert(key).second) {
      continue;
    }
    query_keys.push_back(key);
  }
  for (const std::string& key : CollectDhtQueryPeerKeys(mesh_cfg.bootstrap_peers, {})) {
    if (key.empty() || !seen.insert(key).second) {
      continue;
    }
    query_keys.push_back(key);
  }

  AmpDirectoryProtocolConfig cfg;
  cfg.local_peer_id = mesh_->Amp()->LocalPeerId();
  cfg.query_peer_keys = std::move(query_keys);
  mesh_->ConfigureAmpDirectory(std::move(cfg));
  mesh_->RefreshAmpDirectoryHosting(host_directory);

  if (host_directory) {
    mesh_->AmpDirectory()->SetNodesProvider([this, mesh_cfg]() {
      std::vector<MeshNodeHit> nodes;
      if (identity_ && mesh_) {
        nodes.push_back(BuildLocalMeshNodeHit(*identity_, *mesh_, mesh_cfg));
      }
      if (mesh_directory_cache_) {
        for (const MeshDirectoryNode& row : mesh_directory_cache_->Snapshot()) {
          if (row.peer_id.empty()) {
            continue;
          }
          MeshNodeHit hit;
          hit.relay_user_id = row.account_id.empty() ? row.peer_id : row.account_id;
          if (!row.account_id.empty()) {
            hit.account_id = row.account_id;
          }
          if (!row.nickname.empty()) {
            hit.nickname = row.nickname;
          }
          hit.entity_kind = row.entity_kind.empty() ? "mesh_node" : row.entity_kind;
          hit.seq = row.seq;
          hit.expires_at = row.expires_at;
          hit.capabilities.circuit_relay = row.circuit_relay;
          hit.capabilities.media_relay = row.media_relay;
          hit.capabilities.dht = row.dht;
          hit.capabilities.ledger_gateway = row.ledger_gateway;
          DirectoryEndpoint ep;
          ep.peer_id = row.peer_id;
          ep.multiaddrs = row.multiaddrs;
          hit.endpoints.push_back(std::move(ep));
          nodes.push_back(std::move(hit));
        }
      }
      return nodes;
    });
  }
}

bool ConversationsHub::IsContactReachable(const Contact& contact) const {
  return IsContactReachableForMessaging(contact, relay_ != nullptr);
}


void ConversationsHub::PrefetchPeerReachability(const std::string& identity) {
  if (!messaging_ready_ || identity.empty() || !mesh_messaging_) {
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
  if (mesh_ && mesh_->AmpDht() && ResolveMeshRole(config_.mesh) == MeshRole::Node &&
      config_.mesh.capabilities.dht) {
    mesh_->AmpDht()->FindPeer(peer_id, [this, peer_id](AmpDhtProtocol::FindPeerRoe result) {
      if (!result) {
        return;
      }
      ApplyDhtFindPeerResult(peer_id, result->record);
    });
  }
}


Roe<void> ConversationsHub::Initialize(const AppConfig& config, const std::string& profile_data_dir) {
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
  payment_promises_ = std::make_unique<PaymentPromiseStore>(data_dir_);
  (void)payment_promises_->Load();

  {
    StartupPhase phase("ConversationsHub::ReconcileOutbox");
    (void)store_->ReconcileOutbox();
  }

  InstallOrgBackendClients(config);

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

  if (directory_) {
    mesh_directory_cache_ = std::make_unique<MeshDirectoryCache>([this]() -> Roe<std::vector<MeshDirectoryNode>> {
      // N029 nd4: Amp directory twin first, then HTTP INameDirectory.
      if (mesh_ && mesh_->AmpDirectory() && mesh_->AmpDirectory()->IsStarted()) {
        auto amp_nodes = mesh_->AmpDirectory()->ListMeshNodes();
        if (amp_nodes) {
          auto rows = MeshDirectoryNodesFromHits(*amp_nodes);
          if (!rows.empty()) {
            return rows;
          }
        }
      }
      if (!directory_) {
        return std::vector<MeshDirectoryNode>{};
      }
      DirectoryClientNameDirectory names(*directory_);
      auto records = names.ListService("mesh_node");
      if (!records) {
        return records.error();
      }
      return MeshDirectoryNodesFromNameRecords(*records);
    });
    mesh_directory_cache_->SetOnUpdated([this]() {
      RegisterMeshDirectoryEndpoints();
      ConfigureAmpDhtProtocol();
      ConfigureAmpDirectoryProtocol();
    });
    mesh_directory_cache_->RequestRefresh();
  }

  if (secrets_ != nullptr) {
    secrets_->RegisterDekConsumer(identity_.get());
    secrets_->RegisterDekConsumer(psk_store_.get());
    secrets_->RegisterDekConsumer(static_cast<SqliteThreadStore*>(store_.get()));
    secrets_->RegisterDekConsumer(call_stack_->MediaKeys());
  }
  signing_resolver_ = std::make_unique<RelayDirectorySigningKeyResolver>(signing_key_store_, *directory_);
  kem_resolver_ = std::make_unique<RelayDirectoryKemKeyResolver>(kem_key_store_, *directory_);

  // P2P stack without libp2p until profile unlock (relay-capable once identity exists).
  mesh_messaging_ = std::make_unique<MeshDeliveryOrchestrator>(*store_, *contacts_, *identity_, relay_, *inbox_,
                                                signing_key_store_, *signing_resolver_, kem_key_store_, *kem_resolver_,
                                                *psk_store_, *group_roster_, group_invite_gate_.get());
  mesh_messaging_->SetProfileDataDir(data_dir_);
  mesh_messaging_->SetInitiationBillingStore(initiation_billing_.get());
  mesh_messaging_->SetPaymentPromiseStore(payment_promises_.get());
  mesh_messaging_->SetPeerRouteSources(directory_shadows_.get(), directory_);
  WireAttachmentDownloads();
  group_membership_ = std::make_unique<GroupMembershipWorkflow>(*store_, *contacts_, *identity_, *group_roster_,
                                                               *group_invite_gate_, *mesh_messaging_);
  inbox_->SetGroupMembership(group_membership_.get());
  mesh_messaging_->SetGroupMembership(group_membership_.get());
  call_stack_->BuildSessions(MakeCallStackDeps());
  if (auto* calls = call_stack_->Calls()) {
    calls->SetInitiationBillingStore(initiation_billing_.get());
  }
  actions_ = std::make_unique<ContactActionDispatcher>(*inbox_, *contacts_, *identity_, *store_,
                                                       group_membership_.get(), registration_, mesh_messaging_.get());

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

void ConversationsHub::SetOnMessagingReady(std::function<void()> callback) {
  on_messaging_ready_ = std::move(callback);
}

void ConversationsHub::SetOnMeshReady(std::function<void()> callback) {
  on_mesh_ready_ = std::move(callback);
}

void ConversationsHub::SetOnCallWake(std::function<void()> callback) {
  on_call_wake_ = std::move(callback);
}

void ConversationsHub::NotifyMessagingReady() {
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

void ConversationsHub::NotifyMeshReady() {
  if (!on_mesh_ready_) {
    return;
  }
  if (AppRuntime::CurrentlyOnUI()) {
    on_mesh_ready_();
    return;
  }
  AppRuntime::PostUI([this]() {
    if (on_mesh_ready_) {
      on_mesh_ready_();
    }
  });
}

Roe<void> ConversationsHub::BuildLocalMessagingStack() {
  if (shutdown_requested_.load(std::memory_order_acquire)) {
    return Error("shutdown in progress");
  }
  WireRelayAuthSigner();

  // Local / relay-capable stack: Amp links stay null until AttachAmpMessagingStack.
  IChatPeerLinks* amp_links = nullptr;
  std::function<void()> amp_pump;
  std::function<void(std::function<void()>)> amp_worker;

  mesh_messaging_ = std::make_unique<MeshDeliveryOrchestrator>(
      *store_, *contacts_, *identity_, relay_, *inbox_, signing_key_store_, *signing_resolver_, kem_key_store_,
      *kem_resolver_, *psk_store_, *group_roster_, group_invite_gate_.get(), amp_links, std::move(amp_pump),
      std::move(amp_worker));
  mesh_messaging_->SetProfileDataDir(data_dir_);
  mesh_messaging_->SetInitiationBillingStore(initiation_billing_.get());
  mesh_messaging_->SetPaymentPromiseStore(payment_promises_.get());
  mesh_messaging_->SetPeerRouteSources(directory_shadows_.get(), directory_);
  WireAttachmentDownloads();
  group_membership_ = std::make_unique<GroupMembershipWorkflow>(*store_, *contacts_, *identity_, *group_roster_,
                                                               *group_invite_gate_, *mesh_messaging_);
  inbox_->SetGroupMembership(group_membership_.get());
  mesh_messaging_->SetGroupMembership(group_membership_.get());
  call_stack_->BuildSessions(MakeCallStackDeps());
  if (auto* calls = call_stack_->Calls()) {
    calls->SetInitiationBillingStore(initiation_billing_.get());
  }
  actions_ = std::make_unique<ContactActionDispatcher>(*inbox_, *contacts_, *identity_, *store_,
                                                       group_membership_.get(), registration_, mesh_messaging_.get());
  if (auto prefs = UserPreferences::LoadProfile(data_dir_); prefs) {
    const GroupInvitePolicy policy = GroupInvitePolicyFromString(prefs->group_invite_policy);
    group_invite_gate_->SetInboundPolicy(policy);
    group_membership_->SetInboundPolicy(policy);
  }
  RegisterContactEndpoints();
  if (agent_inbound_.IsBound()) {
    router_ = std::make_unique<MessageRouter>(*inbox_, *mesh_messaging_, agent_inbound_, *store_);
  }
  if (shutdown_requested_.load(std::memory_order_acquire)) {
    DiscardMessagingBringUp();
    return Error("shutdown in progress");
  }
  return {};
}

Roe<void> ConversationsHub::AttachAmpMessagingStack() {
  if (shutdown_requested_.load(std::memory_order_acquire)) {
    return Error("shutdown in progress");
  }
  if (!mesh_) {
    return {};
  }

  IChatPeerLinks* amp_links = nullptr;
  std::function<void()> amp_pump;
  std::function<void(std::function<void()>)> amp_worker;
  if (auto chat = mesh_->ChatDeps(); chat) {
    amp_links = &chat->links;
    amp_pump = std::move(chat->io.io_pump);
    amp_worker = std::move(chat->io.post_worker);
  }
  if (amp_pump && !amp_worker) {
    amp_worker = [](std::function<void()> task) { AppRuntime::PostWorkerNormal(std::move(task)); };
  }

  mesh_messaging_ = std::make_unique<MeshDeliveryOrchestrator>(
      *store_, *contacts_, *identity_, relay_, *inbox_, signing_key_store_, *signing_resolver_, kem_key_store_,
      *kem_resolver_, *psk_store_, *group_roster_, group_invite_gate_.get(), amp_links, std::move(amp_pump),
      std::move(amp_worker));
  mesh_messaging_->SetProfileDataDir(data_dir_);
  mesh_messaging_->SetInitiationBillingStore(initiation_billing_.get());
  mesh_messaging_->SetPaymentPromiseStore(payment_promises_.get());
  mesh_messaging_->SetPeerRouteSources(directory_shadows_.get(), directory_);
  WireAttachmentDownloads();
  group_membership_ = std::make_unique<GroupMembershipWorkflow>(*store_, *contacts_, *identity_, *group_roster_,
                                                               *group_invite_gate_, *mesh_messaging_);
  inbox_->SetGroupMembership(group_membership_.get());
  mesh_messaging_->SetGroupMembership(group_membership_.get());
  call_stack_->BuildSessions(MakeCallStackDeps());
  if (auto* calls = call_stack_->Calls()) {
    calls->SetInitiationBillingStore(initiation_billing_.get());
  }
  actions_ = std::make_unique<ContactActionDispatcher>(*inbox_, *contacts_, *identity_, *store_,
                                                       group_membership_.get(), registration_, mesh_messaging_.get());
  if (auto prefs = UserPreferences::LoadProfile(data_dir_); prefs) {
    const GroupInvitePolicy policy = GroupInvitePolicyFromString(prefs->group_invite_policy);
    group_invite_gate_->SetInboundPolicy(policy);
    group_membership_->SetInboundPolicy(policy);
  }
  RegisterContactEndpoints();
  if (agent_inbound_.IsBound()) {
    router_ = std::make_unique<MessageRouter>(*inbox_, *mesh_messaging_, agent_inbound_, *store_);
  }
  if (shutdown_requested_.load(std::memory_order_acquire)) {
    DiscardMessagingBringUp();
    return Error("shutdown in progress");
  }
  return {};
}

Roe<void> ConversationsHub::BuildMessagingStack() {
  // Legacy sync path (tests / callers that still want local+mesh together).
  if (auto local = BuildLocalMessagingStack(); !local) {
    return local.error();
  }
  if (auto mesh_start = StartMesh(config_); !mesh_start) {
    if (shutdown_requested_.load(std::memory_order_acquire)) {
      return mesh_start.error();
    }
    log().warning << "mesh host start failed: " << mesh_start.error().message
                  << " (direct P2P unavailable; relay messaging may still work)";
  }
  if (shutdown_requested_.load(std::memory_order_acquire)) {
    DiscardMessagingBringUp();
    return Error("shutdown in progress");
  }
  return AttachAmpMessagingStack();
}

void ConversationsHub::ScheduleMeshBringUp() {
  if (mesh_bringup_scheduled_.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  AppRuntime::PostWorkerNormal([this]() {
    StartupPhase phase("ConversationsHub::MeshBringUpAsync");
    if (shutdown_requested_.load(std::memory_order_acquire)) {
      return;
    }
    if (auto mesh_start = StartMesh(config_); !mesh_start) {
      if (shutdown_requested_.load(std::memory_order_acquire)) {
        return;
      }
      log().warning << "async mesh host start failed: " << mesh_start.error().message
                    << " (direct P2P unavailable; relay messaging may still work)";
    }
    if (shutdown_requested_.load(std::memory_order_acquire)) {
      return;
    }
    // Attach Amp + flip mesh_ready on the UI thread (orchestrator is UI-touched).
    AppRuntime::PostUI([this]() {
      if (shutdown_requested_.load(std::memory_order_acquire) || !messaging_ready_) {
        return;
      }
      if (mesh_) {
        if (auto attached = AttachAmpMessagingStack(); !attached) {
          log().warning << "AttachAmpMessagingStack failed: " << attached.error().message;
          mesh_ready_ = false;
        } else {
          mesh_ready_ = true;
        }
      } else {
        mesh_ready_ = false;
      }
      NotifyMeshReady();
    });
  });
}

Roe<void> ConversationsHub::EnsureMessagingReady() {
  StartupPhase phase("ConversationsHub::EnsureMessagingReady");
  if (shutdown_requested_.load(std::memory_order_acquire)) {
    return Error("shutdown in progress");
  }
  if (!initialized_) {
    return AppError::Pin(Err::Pin::Required, "Messaging hub not initialized");
  }
  if (messaging_ready_) {
    if (!mesh_bringup_scheduled_.load(std::memory_order_acquire) && !mesh_ready_) {
      ScheduleMeshBringUp();
    }
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

  // Interactive-ready = local stack only. Mesh (incl. Node reachability probe) is async.
  if (auto built = BuildLocalMessagingStack(); !built) {
    return built.error();
  }
  if (shutdown_requested_.load(std::memory_order_acquire)) {
    DiscardMessagingBringUp();
    return Error("shutdown in progress");
  }

  messaging_ready_ = true;
  mesh_ready_ = false;
  StartCoordinatorTimers();
  NotifyMessagingReady();
  ScheduleMeshBringUp();
  return {};
}

Roe<void> ConversationsHub::Reinitialize(const AppConfig& config, const std::string& profile_data_dir) {
  if (!initialized_) {
    return Initialize(config, profile_data_dir);
  }

  config_ = config;
  UpdateOrgBackendClients(config);
  if (mesh_messaging_) {
    mesh_messaging_->SetRelayClient(relay_);
  }
  if (actions_) {
    actions_->SetRegistrationClient(registration_);
  }
  return {};
}

void ConversationsHub::BindAgentInbound(AgentInboundPorts ports) {
  agent_inbound_ = std::move(ports);
  if (mesh_messaging_ && agent_inbound_.IsBound()) {
    router_ = std::make_unique<MessageRouter>(*inbox_, *mesh_messaging_, agent_inbound_, *store_);
  }
}

void ConversationsHub::BindSessionStore(SessionStore& store) {
  session_store_ = &store;
}

void ConversationsHub::BindSecrets(ProfileSecretsEngine& secrets) {
  secrets_ = &secrets;
}

PeerSigningKeyStore& ConversationsHub::SigningKeys() {
  return signing_key_store_;
}

void ConversationsHub::TickAmpMesh() {
  if (!messaging_ready_ || !mesh_) {
    return;
  }
  mesh_->Tick();
}

void ConversationsHub::TickMesh() {
  if (!messaging_ready_) {
    return;
  }
  // Mesh UDP drain is on amp_mesh_pump_timer_ (~5ms). Policy stays on the 1s timer.
  if (mesh_messaging_) {
    mesh_messaging_->TickMesh();
  }
  if (mesh_directory_cache_) {
    mesh_directory_cache_->MaybeRefresh();
  }
  if (mesh_ && mesh_->AmpDht()) {
    mesh_->AmpDht()->Tick();
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

void ConversationsHub::StartCoordinatorTimers() {
  if (amp_mesh_pump_timer_id_ == 0 && mesh_) {
    amp_mesh_pump_timer_id_ =
        AppRuntime::ScheduleCoordinatorRepeating(kAmpMeshPumpInterval, [this]() { TickAmpMesh(); });
  }
  if (hub_policy_timer_id_ == 0) {
    hub_policy_timer_id_ = AppRuntime::ScheduleCoordinatorRepeating(kHubPolicyTimerInterval, [this]() {
      TickMesh();
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
    if (mesh_messaging_) {
      mesh_messaging_->SyncInboxFromWake(force);
    }
  });
  BackgroundSyncScheduler::Instance().RequestWakeSync();
}

void ConversationsHub::StopCoordinatorTimers() {
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

ReachabilitySnapshot ConversationsHub::Reachability() const {
  return mesh_ ? mesh_->Reachability().Snapshot() : ReachabilitySnapshot{};
}

bool ConversationsHub::IsHelpNetworkEnabled() const {
  return ResolveMeshRole(config_.mesh) == MeshRole::Node;
}

void ConversationsHub::SetOnReachabilityUpdated(std::function<void()> callback) {
  on_reachability_updated_ = std::move(callback);
}

void ConversationsHub::SetOnPeerIconsChanged(std::function<void()> callback) {
  on_peer_icons_changed_ = std::move(callback);
}

void ConversationsHub::NotifyPeerIconsChanged() {
  if (on_peer_icons_changed_) {
    on_peer_icons_changed_();
  }
}

std::string ConversationsHub::ContactIconLocalPath(const Contact& contact) {
  const std::string key = ProfileIconCacheKeyForContact(contact);
  if (key.empty()) {
    return {};
  }
  return ProfileIconLocalPath(data_dir_, key);
}

std::string ConversationsHub::IdentityIconLocalPath(const std::string& identity) {
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

void ConversationsHub::EnsureDirectoryHitIconCached(const DirectoryHit& hit) {
  ScheduleDirectoryHitIconFetch(hit);
}

void ConversationsHub::EnsureContactIconCached(const Contact& contact) {
  ScheduleContactIconFetch(contact);
}

void ConversationsHub::ScheduleDirectoryHitIconFetch(const DirectoryHit& hit) {
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

void ConversationsHub::ScheduleContactIconFetch(const Contact& contact) {
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

void ConversationsHub::RunReachabilityProbe(bool try_upnp) {
  if (!mesh_) {
    return;
  }
  mesh_->StartReachabilityProbe(try_upnp);
}

void ConversationsHub::TryUpnpPortMapping() {
  if (!mesh_) {
    return;
  }
  // Amp listen uses UDP; mapping is attempted inside the reachability probe when try_upnp is set.
  mesh_->StartReachabilityProbe(true);
}



ProfileIdentityView ConversationsHub::LoadProfileIdentityView() {
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

Roe<void> ConversationsHub::SaveProfileNickname(const std::string& nickname) {
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

Roe<void> ConversationsHub::RegisterIdentity(const std::string& nickname) {
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

Roe<void> ConversationsHub::UploadProfileIconFromPath(const std::string& path) {
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

Roe<void> ConversationsHub::ClearProfileIcon() {
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

Roe<BlobQuotaRecoveryPlan> ConversationsHub::PlanRelayQuotaRecovery() {
  if (!IsInitialized()) {
    return Error("Messaging hub not initialized");
  }
  if (!IsMessagingReady()) {
    return AppError::Pin(Err::Pin::Required, "Unlock profile PIN to manage relay blob storage");
  }
  if (!blob_) {
    return Error("Blob client not configured");
  }
  auto identity = Identity().Get();
  if (!identity) {
    return identity.error();
  }
  if (!identity->registered || identity->relay_user_id.empty()) {
    return Error("Register on the network before using relay blob storage");
  }
  return PlanOldestRelayBlobDeletion(*blob_, identity->relay_user_id, ProtectedRelayBlobId(data_dir_));
}

Roe<void> ConversationsHub::FreeOldestRelayBlobSlot() {
  if (!IsInitialized()) {
    return Error("Messaging hub not initialized");
  }
  if (!IsMessagingReady()) {
    return AppError::Pin(Err::Pin::Required, "Unlock profile PIN to manage relay blob storage");
  }
  if (!blob_) {
    return Error("Blob client not configured");
  }
  auto identity = Identity().Get();
  if (!identity) {
    return identity.error();
  }
  if (!identity->registered || identity->relay_user_id.empty()) {
    return Error("Register on the network before using relay blob storage");
  }
  return pbr::FreeOldestRelayBlobSlot(*blob_, identity->relay_user_id, ProtectedRelayBlobId(data_dir_));
}

Roe<ThreadMessage> ConversationsHub::SendAttachmentFromPath(const std::string& thread_id, const std::string& path) {
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
  upload_opts.peer_client = mesh_messaging_ ? mesh_messaging_->PeerBlobClient() : nullptr;
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

  if (active.kind == ThreadKind::Group) {
    auto sent = MeshMessaging().SendGroupMessage(thread_id, display, opts);
    if (sent) {
      if (!attachment_dek.empty() && !profile_id_.empty()) {
        (void)CopyAttachmentPlaintextFile(data_dir_, thread_id, *fields, path, attachment_dek, profile_id_);
      }
      Attachments().MaybeBuildPoster(thread_id, *fields);
      if (attachment_downloads_) {
        attachment_downloads_->EnqueueFromMessage(thread_id, *sent);
      }
    }
    return sent;
  }
  auto sent = MeshMessaging().SendUserMessage(thread_id, display, opts);
  if (sent) {
    if (!attachment_dek.empty() && !profile_id_.empty()) {
      (void)CopyAttachmentPlaintextFile(data_dir_, thread_id, *fields, path, attachment_dek, profile_id_);
    }
    Attachments().MaybeBuildPoster(thread_id, *fields);
    if (attachment_downloads_) {
      attachment_downloads_->EnqueueFromMessage(thread_id, *sent);
    }
  }
  return sent;
}

AttachmentFetchWorkflow& ConversationsHub::Attachments() {
  if (!attachment_downloads_) {
    attachment_downloads_ = std::make_unique<AttachmentFetchWorkflow>();
    attachment_downloads_->SetProfileDataDir(data_dir_);
    attachment_downloads_->SetProfileId(profile_id_);
  }
  return *attachment_downloads_;
}

void ConversationsHub::WireAttachmentDownloads() {
  if (!attachment_suppressions_) {
    attachment_suppressions_ = std::make_unique<AttachmentSuppressionStore>(data_dir_);
  } else {
    attachment_suppressions_->SetProfileDir(data_dir_);
  }
  if (!attachment_downloads_) {
    attachment_downloads_ = std::make_unique<AttachmentFetchWorkflow>();
  }
  attachment_downloads_->SetProfileDataDir(data_dir_);
  attachment_downloads_->SetProfileId(profile_id_);
  attachment_downloads_->SetFetchDependencies(&Store(), contacts_.get(), identity_.get(),
                                                  mesh_messaging_ ? mesh_messaging_->PeerBlobClient() : nullptr);
  attachment_downloads_->SetSuppressionStore(attachment_suppressions_.get());
  if (auto prefs = UserPreferences::LoadProfile(data_dir_); prefs) {
    attachment_downloads_->SetDownloadPolicy(AttachmentDownloadPolicyFromString(prefs->attachment_download_policy));
  }
  attachment_downloads_->SetOnChanged([this]() {
    if (mesh_messaging_) {
      mesh_messaging_->NotifyMessagesChanged();
    }
  });
  if (inbox_) {
    inbox_->SetProfileDataDir(data_dir_);
    inbox_->SetAttachmentDownloads(attachment_downloads_.get());
  }
  if (mesh_messaging_) {
    mesh_messaging_->SetAttachmentDownloads(attachment_downloads_.get());
    if (auto* peer_blob = mesh_messaging_->PeerBlobService()) {
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

void ConversationsHub::RequestAttachmentDownload(const std::string& thread_id, const std::string& message_id) {
  if (!IsInitialized() || thread_id.empty() || message_id.empty()) {
    return;
  }
  Attachments().RequestDownload(thread_id, message_id, Store());
}

void ConversationsHub::DrainPendingAttachmentMedia() {
  if (!IsInitialized()) {
    return;
  }
  Attachments().DrainPendingMediaBacklog(Store());
}

Roe<void> ConversationsHub::ClearDownloadedAttachments() {
  if (!IsInitialized()) {
    return Error("Messaging hub not initialized");
  }
  auto cleared = Attachments().ClearAllDownloadedMedia(Store());
  if (cleared && mesh_messaging_) {
    mesh_messaging_->NotifyMessagesChanged();
  }
  return cleared;
}

Roe<void> ConversationsHub::SendChargeRequired(const std::string& peer_identity,
                                           const std::optional<int64_t> floor_minor) {
  if (!IsInitialized() || !IsMessagingReady() || !mesh_messaging_) {
    return Error("Messaging not ready");
  }
  return mesh_messaging_->SendChargeRequired(peer_identity, floor_minor);
}

Roe<PaymentPromise> ConversationsHub::CreatePaymentPromiseOffer(const PaymentPromiseLifecycle::OfferParams& params) {
  if (!payment_promises_) {
    return Error("payment promise store unavailable");
  }
  return PaymentPromiseLifecycle::CreateOffer(*payment_promises_, Identity(), params);
}

Roe<PaymentPromise> ConversationsHub::CreatePaymentPromiseOfferForThread(const std::string& thread_id,
                                                                     PaymentPromiseLifecycle::OfferParams params) {
  if (thread_id.empty()) {
    return Error("thread_id required");
  }
  params.service_ref = "thread:" + thread_id;
  params.release_rule = PaymentPromiseReleaseRule::PayerAck;
  return CreatePaymentPromiseOffer(params);
}

Roe<PaymentPromise> ConversationsHub::AcceptPaymentPromise(const std::string& promise_id) {
  if (!payment_promises_) {
    return Error("payment promise store unavailable");
  }
  return PaymentPromiseLifecycle::Accept(*payment_promises_, Identity(), promise_id);
}

Roe<PaymentPromise> ConversationsHub::MarkPaymentPromiseDelivering(const std::string& promise_id) {
  if (!payment_promises_) {
    return Error("payment promise store unavailable");
  }
  return PaymentPromiseLifecycle::MarkDelivering(*payment_promises_, promise_id);
}

Roe<PaymentPromise> ConversationsHub::RecordPaymentPromiseOutcome(const std::string& promise_id,
                                                             const PaymentPromiseState outcome,
                                                             const std::string& note) {
  if (!payment_promises_) {
    return Error("payment promise store unavailable");
  }
  return PaymentPromiseLifecycle::RecordOutcome(*payment_promises_, Identity(), promise_id, outcome, note);
}

Roe<void> ConversationsHub::AvoidPaymentPromiseCounterparty(const std::string& promise_id) {
  if (!payment_promises_) {
    return Error("payment promise store unavailable");
  }
  return PaymentPromiseLifecycle::AvoidCounterparty(*payment_promises_, Contacts(), Identity(), promise_id);
}

Roe<std::vector<PaymentPromise>> ConversationsHub::ListPaymentPromises() const {
  if (!payment_promises_) {
    return Error("payment promise store unavailable");
  }
  return payment_promises_->List();
}

Roe<std::optional<PaymentPromise>> ConversationsHub::GetPaymentPromise(const std::string& promise_id) const {
  if (!payment_promises_) {
    return Error("payment promise store unavailable");
  }
  return payment_promises_->Get(promise_id);
}

Roe<std::vector<PaymentPromise>> ConversationsHub::ListPendingInboundPaymentPromises() const {
  if (!payment_promises_) {
    return Error("payment promise store unavailable");
  }
  return payment_promises_->ListPendingInbound();
}

Roe<std::optional<PaymentPromise>> ConversationsHub::GetPendingInboundPaymentPromise(
    const std::string& promise_id) const {
  if (!payment_promises_) {
    return Error("payment promise store unavailable");
  }
  return payment_promises_->GetPendingInbound(promise_id);
}

Roe<PaymentPromise> ConversationsHub::AcceptInboundPaymentPromise(const std::string& promise_id) {
  if (!payment_promises_) {
    return Error("payment promise store unavailable");
  }
  return payment_promises_->AcceptInbound(promise_id);
}

Roe<bool> ConversationsHub::IgnoreInboundPaymentPromise(const std::string& promise_id) {
  if (!payment_promises_) {
    return Error("payment promise store unavailable");
  }
  return payment_promises_->IgnoreInbound(promise_id);
}

bool ConversationsHub::ShouldAvoidPaymentCounterparty(const std::string& other_account_id) {
  if (!payment_promises_ || other_account_id.empty()) {
    return false;
  }
  auto local = Identity().GetAccountId();
  if (!local) {
    return false;
  }
  return PaymentPromiseAvoid::ShouldAvoid(*payment_promises_, Contacts(), *local, other_account_id);
}

Roe<ThreadMessage> ConversationsHub::BuildPaymentPromiseControlMessage(const std::string& thread_id,
                                                                   const PaymentPromiseControlType type,
                                                                   const PaymentPromise& promise,
                                                                   const std::string& body_text) {
  return PaymentPromiseWireCodec::BuildSystemMessage(thread_id, type, promise, body_text);
}

Roe<PaymentPromise> ConversationsHub::StagePaymentPromiseControlMessage(const ThreadMessage& message) {
  if (!payment_promises_) {
    return Error("payment promise store unavailable");
  }
  auto type = PaymentPromiseWireCodec::ControlTypeFromMessage(message);
  if (!type) {
    return Error("not a payment promise control message");
  }
  auto root = TryParseObject(message.payload_json);
  if (!root) {
    return Error("invalid payment promise control payload");
  }
  auto detail = root->getString("detail");
  if (!detail || detail->empty()) {
    return Error("payment promise control missing detail");
  }
  auto decoded = PaymentPromiseWireCodec::DecodeDetail(*detail);
  if (!decoded) {
    return decoded.error();
  }
  // P003: stage only — never auto-commit remote receipts.
  auto staged = payment_promises_->StageInbound(*decoded);
  if (!staged) {
    return staged.error();
  }
  return *decoded;
}

Roe<void> ConversationsHub::RotateBriefLlmKey() {
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

void ConversationsHub::TickReachabilityUx() {
  if (!config_.mesh.node_enabled || Platform::IsMobile()) {
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

void ConversationsHub::RefreshMeshCapabilities() {
  if (!messaging_ready_ || !mesh_) {
    return;
  }
  if (session_store_ && session_store_->IsInitialized()) {
    config_.mesh = session_store_->Snapshot().config.mesh;
  }
  const MeshRole role = ResolveMeshRole(config_.mesh);
  // Amp L4 inbound hosting is gated via SetServeInbound (no TCP CircuitRelay/MediaRelay).
  if (mesh_->AmpCircuitTunnel()) {
    mesh_->AmpCircuitTunnel()->SetServeInbound(role == MeshRole::Node &&
                                               config_.mesh.capabilities.circuit_relay);
  }
  call_stack_->ResetRelayClients();
  if (mesh_->AmpMediaRelayCoord()) {
    mesh_->AmpMediaRelayCoord()->SetServeInbound(role == MeshRole::Node &&
                                                 config_.mesh.capabilities.media_relay);
  }
  ConfigureAmpDhtProtocol();
  ConfigureAmpDirectoryProtocol();
  ApplyMeshAdmissionPolicies();
  call_stack_->WireMediaRelayDeps();
  SyncMobileEphemeralListen();
}



void ConversationsHub::Apply(const NetworkConfig& next) {
  if (!initialized_) {
    return;
  }

  const bool service_urls_changed = next.relay.base_url != config_.relay.base_url ||
                                    next.directory.base_url != config_.directory.base_url ||
                                    next.directory.transport != config_.directory.transport ||
                                    next.directory.providers != config_.directory.providers ||
                                    next.registration.base_url != config_.registration.base_url;
  const bool mesh_changed =
      next.node_enabled != config_.mesh.node_enabled ||
      next.circuit_relay != config_.mesh.capabilities.circuit_relay ||
      next.media_relay != config_.mesh.capabilities.media_relay ||
      next.dht != config_.mesh.capabilities.dht ||
      next.prefer_contacts_for_routing != config_.mesh.prefer_contacts_for_routing;

  config_.relay = next.relay;
  config_.directory = next.directory;
  config_.registration = next.registration;
  config_.mesh.node_enabled = next.node_enabled;
  config_.mesh.capabilities.circuit_relay = next.circuit_relay;
  config_.mesh.capabilities.media_relay = next.media_relay;
  config_.mesh.capabilities.dht = next.dht;
  config_.mesh.prefer_contacts_for_routing = next.prefer_contacts_for_routing;

  if (service_urls_changed) {
    UpdateOrgBackendClients(config_);
    if (mesh_messaging_) {
      mesh_messaging_->SetRelayClient(relay_);
    }
    if (actions_) {
      actions_->SetRegistrationClient(registration_);
    }
  }
  if (mesh_changed && messaging_ready_) {
    RefreshMeshCapabilities();
  }
}

void ConversationsHub::Apply(const PolicyPrefs& prefs) {
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

void ConversationsHub::Apply(const NotificationPrefs& prefs) {
  if (!initialized_ || !messaging_ready_) {
    return;
  }
  (void)PushDeviceCoordinator::SyncWithPreference(*this, prefs.show_notifications);
}

ConversationsHub::NetworkConfig ConversationsHub::ProjectNetwork(const AppConfig& config) {
  NetworkConfig out;
  out.relay = config.relay;
  out.directory = config.directory;
  out.registration = config.registration;
  out.node_enabled = config.mesh.node_enabled;
  out.circuit_relay = config.mesh.capabilities.circuit_relay;
  out.media_relay = config.mesh.capabilities.media_relay;
  out.dht = config.mesh.capabilities.dht;
  out.prefer_contacts_for_routing = config.mesh.prefer_contacts_for_routing;
  return out;
}

ConversationsHub::PolicyPrefs ConversationsHub::ProjectPolicy(const ProfilePreferences& prefs) {
  return {.group_invite_policy = GroupInvitePolicyFromString(prefs.group_invite_policy),
          .attachment_download_policy = AttachmentDownloadPolicyFromString(prefs.attachment_download_policy)};
}

ConversationsHub::NotificationPrefs ConversationsHub::ProjectNotifications(const ProfilePreferences& prefs) {
  return {.show_notifications = prefs.show_notifications};
}

Roe<CircuitRelayBridgeResult> ConversationsHub::RequestCircuitBridgePreferred(const std::string& target_peer_id,
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
  MeshConfig mesh_cfg = config_.mesh;
  NormalizeMeshConfig(mesh_cfg);
  const bool include_seeds = !mesh_ || mesh_->Reachability().Snapshot().signals.seed_dial_ok;
  std::vector<MeshDirectoryNode> directory_nodes;
  if (mesh_directory_cache_) {
    directory_nodes = mesh_directory_cache_->Snapshot();
  }
  std::vector<MeshDirectoryNode> dht_nodes;
  if (mesh_ && mesh_->AmpDht()) {
    dht_nodes = MeshDirectoryNodesFromDhtRecords(mesh_->AmpDht()->SnapshotRecords());
  }
  auto hops = BuildCircuitHopList(contacts, directory_nodes, dht_nodes, mesh_cfg.bootstrap_peers,
                                 mesh_cfg.prefer_contacts_for_routing, include_seeds);
  if (hops.empty()) {
    return Error("no circuit hop candidates");
  }

  CircuitBridgeTarget target;
  target.target_peer_id = target_peer_id;
  target.target_multiaddr = target_multiaddr;

  CircuitRelayBridgeResult last;
  last.error = "all hops failed";

  auto circuit_deps = mesh_->CircuitDeps();
  if (!circuit_deps) {
    return Error("Amp circuit deps unavailable");
  }
  IChatPeerLinks& links = circuit_deps->links;
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


void ConversationsHub::SuspendMeshColdPeers() {
  SyncMobileEphemeralListen();
}


void ConversationsHub::Shutdown() {
  if (!initialized_) {
    return;
  }

  shutdown_requested_.store(true, std::memory_order_release);

  on_messaging_ready_ = nullptr;
  on_reachability_updated_ = nullptr;
  agent_inbound_ = {};

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
  if (mesh_messaging_) {
    mesh_messaging_->SetOnMessagesChanged(nullptr);
    mesh_messaging_->SetOnDeliveryNotice(nullptr);
    mesh_messaging_->SetOnBackgroundUnread(nullptr);
    mesh_messaging_->SetGroupMembership(nullptr);
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
  // Unregister DEK consumers before StopMesh → DetachAmpTransports destroys peer_blob
  // (otherwise ProfileSecretsEngine::Shutdown ClearDek UAFs the stale pointer).
  if (secrets_ != nullptr) {
    if (attachment_downloads_) {
      secrets_->UnregisterDekConsumer(attachment_downloads_.get());
    }
    if (mesh_messaging_) {
      if (auto* peer_blob = mesh_messaging_->PeerBlobService()) {
        secrets_->UnregisterDekConsumer(peer_blob);
      }
    }
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
  // Stop libp2p / Connect workers before dropping session façade (Leave may still be dialing).
  StopMesh();
  // Drop the call session manager before P2P — CSM holds a MeshDeliveryOrchestrator& reference.
  call_stack_->ResetSessions();
  // Destroy P2P before groups — P2P held a non-owning Groups pointer.
  mesh_messaging_.reset();
  group_membership_.reset();
  group_invite_gate_.reset();
  group_roster_.reset();
  signing_resolver_.reset();
  kem_resolver_.reset();
  // Reset media engine / key store / session store after DEK unregister above.
  call_stack_->Shutdown();
  psk_store_.reset();
  inbox_.reset();
  peer_labels_.reset();
  directory_shadows_.reset();
  mesh_directory_cache_.reset();
  http_relay_.reset();
  directory_owned_.reset();
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
  mesh_ready_ = false;
  mesh_bringup_scheduled_.store(false, std::memory_order_release);
  initialized_ = false;
}

InboxController& ConversationsHub::Inbox() {
  return *inbox_;
}

MeshDeliveryOrchestrator& ConversationsHub::MeshMessaging() {
  return *mesh_messaging_;
}

GroupMembershipWorkflow& ConversationsHub::Groups() {
  return *group_membership_;
}

CallSessionManager* ConversationsHub::Calls() {
  return call_stack_->Calls();
}

CallLifecycle* ConversationsHub::Lifecycle() {
  return call_stack_->Lifecycle();
}

MessageRouter& ConversationsHub::Router() {
  return *router_;
}

ContactActionDispatcher& ConversationsHub::Actions() {
  return *actions_;
}

IThreadStore& ConversationsHub::Store() {
  return *store_;
}

ContactsStore& ConversationsHub::Contacts() {
  return *contacts_;
}

IdentityStore& ConversationsHub::Identity() {
  return *identity_;
}

IPskSessionStore* ConversationsHub::PskStore() {
  return psk_store_.get();
}

ProfileSecretsEngine* ConversationsHub::Secrets() {
  return secrets_;
}

IDirectoryClient& ConversationsHub::Directory() {
  return *directory_;
}

DirectoryShadowCache& ConversationsHub::DirectoryShadows() {
  return *directory_shadows_;
}

PeerDisplayResolver& ConversationsHub::PeerLabels() {
  return *peer_labels_;
}

IRegistrationClient& ConversationsHub::Registration() {
  return *registration_;
}

IBlobClient& ConversationsHub::Blob() {
  return *blob_;
}

IPushDeviceClient* ConversationsHub::PushDevices() {
  return push_devices_;
}

IClientCompatClient* ConversationsHub::ClientCompat() {
  return client_compat_;
}

} // namespace pbr
