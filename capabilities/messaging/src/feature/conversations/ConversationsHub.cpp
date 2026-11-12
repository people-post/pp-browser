#include "feature/conversations/ConversationsHub.h"

#include "lib/messaging/GroupLinkJoinFamily.h"
#include "lib/people/DevicePeerId.h"
#include "domain/attachments/AttachmentContent.h"
#include "domain/attachments/AttachmentFetch.h"
#include "common/chat/MessagingLimits.h"
#include "domain/attachments/AttachmentMetrics.h"
#include "lib/delivery/AttachmentContentCipher.h"
#include "lib/messaging/DeleteForEveryone.h"
#include "lib/messaging/EditMessage.h"
#include "foundation/runtime/AppRuntimeMeshExecutors.h"

#include "lib/people/RecoveryBundle.h"
#include "lib/people/LinkDeviceCoordinator.h"
#include "foundation/crypto/RecoveryPhrase.h"
#include "feature/conversations/MeshConnectivityWiring.h"
#include "common/ProtocolIds.h"
#include "lib/pricing/PaymentPromiseLifecycle.h"
#include "lib/pricing/PaymentPromiseAvoid.h"
#include "lib/pricing/PaymentPromiseWireCodec.h"
#include "lib/pricing/PricingControlFamilies.h"
#include "lib/messaging/GroupRosterSyncFamily.h"
#include "foundation/crypto/CryptoUtil.h"
#include "common/directory/DirectoryJson.h"
#include "common/ValueJson.h"

#include "lib/people/ContactReachability.h"
#include "lib/messaging/GroupInviteGate.h"
#include "lib/messaging/GroupInviteStanding.h"
#include "domain/attachments/AttachmentFetchWorkflow.h"
#include "feature/conversations/GroupMembershipWorkflow.h"
#include "domain/peers/AccountKeyResolver.h"
#include "feature/conversations/ContactCardExchange.h"
#include "feature/conversations/ContactProfileRefresh.h"
#include "lib/messaging/ContactLocator.h"
#include "lib/delivery/PeerRequestAuth.h"
#include "lib/people/PeerCards.h"

#include "feature/conversations/PushDeviceCoordinator.h"
#include "foundation/crypto/ProfileSecretsEngine.h"
#include "foundation/error/AppError.h"
#include "common/Localized.h"
#include "common/MultiaddrPeerId.h"
#include "lib/messaging/AttachmentCache.h"
#include "lib/delivery/DirectChatTargetFromContact.h"
#include "lib/delivery/ChatPayloadCodec.h"
#include "lib/messaging/GroupTypes.h"
#include "lib/delivery/SendRelayOptions.h"
#include "domain/attachments/AttachmentClient.h"
#include "lib/net/BlobQuota.h"
#include "lib/net/HttpClient.h"
#include "lib/net/ProfileIconClient.h"
#include "lib/net/ProfileIconFetch.h"
#include "domain/registration/RegistrationClient.h"
#include "lib/people/ProfileIconCache.h"
#include "lib/people/ContactIdentity.h"
#include "lib/people/ContactTypes.h"
#include "foundation/runtime/AppLifecycle.h"
#include "foundation/runtime/BackgroundSyncScheduler.h"
#include "common/runtime/AppRuntime.h"
#include "feature/conversations/MeshNetworkReaction.h"
#include "mesh/l4/circuit/CircuitBridgeTarget.h"
#include "mesh/l4/circuit/CircuitRelayTypes.h"
#include "mesh/l4/media_relay/MediaRelayTypes.h"
#include "mesh/l4/circuit/client/CircuitClientCoordinator.h"
#include "mesh/l4/circuit/serve/CircuitRelayServer.h"
#include "mesh/reachability/LanMdnsDiscovery.h"
#include "mesh/reachability/AmpObservedAddrs.h"
#include "common/SettledWait.h"
#include "lib/people/ContactAddressDisclosure.h"
#include "lib/people/MeshHopPolicy.h"
#include "mesh/dht/DhtRecordCodec.h"
#include "mesh/discovery/AmpDirectoryProtocol.h"
#include "mesh/discovery/NameDirectory.h"
#include "mesh/host/MeshPorts.h"
#include "mesh/reachability/NatTraversal.h"
#include "mesh/reach/PeerReachSelect.h"
#include "mesh/reachability/Reachability.h"
#include "common/StartupTiming.h"
#include "common/Utilities.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <unordered_set>

#include "common/ValueJson.h"
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

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
  view.brief_llm_key_masked = MaskBriefLlmApiKey(identity.brief_llm_api_key.Reveal());
}

} // namespace

ConversationsHub::ConversationsHub() {
  redirectLogger("ConversationsHub");
}

MeshConnectivityWiringInputs ConversationsHub::MeshInputs() {
  MeshConnectivityWiringInputs reach;
  reach.mesh = [this]() { return mesh_; };
  reach.contacts = contacts_.get();
  reach.mesh_config = [this]() { return MeshConfigSnapshot(); };
  reach.list_directory_nodes = [this]() { return DirectoryNodes(); };
  reach.list_dht_nodes = [this]() {
    if (!mesh_ || !mesh_->AmpDht()) {
      return std::vector<mesh::MeshDirectoryNode>{};
    }
    return mesh::MeshDirectoryNodesFromDhtRecords(mesh_->AmpDht()->SnapshotRecords());
  };
  reach.seed_dial_ok = [this]() {
    if (!mesh_) {
      return true;
    }
    // Only a seed dial that failed rules the seeds out. A device that never probed (client role —
    // every phone — or a node before its first probe) has not learned they are unreachable.
    const mesh::ReachabilitySignals signals = mesh_->Reachability().Snapshot().signals;
    return signals.seed_dial_ok || signals.seed_dial_error.empty();
  };
  reach.node_reputation = [this]() -> std::shared_ptr<MeshNodeReputation> {
    return mesh_owner_ ? mesh_owner_->NodeReputation() : nullptr;
  };
  reach.observe_node = [this](const std::string& peer_id) {
    ObservedNode seen;
    if (mesh_ && mesh_->Amp()) {
      if (auto told = mesh_->Amp()->Links().RemoteCapabilityExtension(peer_id)) {
        seen.told = true;
        seen.amp_wire = told->amp_wire;
        seen.node_version = told->node_version;
        seen.features = told->features;
      }
    }
    if (mesh_owner_) {
      seen.answered_other_wire =
          mesh_owner_->Connectivity().ReachFacts(peer_id).last_failure == mesh::PeerReachFailure::VersionMismatch;
    }
    return seen;
  };
  reach.note_lan_peer_id = [this](const std::string& peer_id) { NoteLanMdnsPeerId(peer_id); };
  reach.register_direct_endpoint = [this](const std::string& identity, const std::string& multiaddr) {
    if (mesh_messaging_) {
      mesh_messaging_->RegisterPeerDirectEndpoint(identity, multiaddr);
    }
  };
  return reach;
}

void ConversationsHub::NoteLanMdnsPeerId(const std::string& peer_id) {
  // Called by mesh connectivity on the mesh's io lane: the id set is the hub's (UI).
  AppRuntime::PostUI([this, peer_id]() { lan_mdns_contact_peer_ids_.insert(peer_id); });
}

void ConversationsHub::WireMeshConnectivity() {
  if (mesh_owner_) {
    mesh_owner_->SetMeshInputs(MeshInputs());
  }
}

void ConversationsHub::AddParticipant(HubParticipant& participant) {
  std::lock_guard lock(participants_mu_);
  participants_.push_back(&participant);
}

void ConversationsHub::ClearParticipants() {
  std::lock_guard lock(participants_mu_);
  participants_.clear();
}

std::vector<HubParticipant*> ConversationsHub::Participants() const {
  std::lock_guard lock(participants_mu_);
  return participants_;
}

std::vector<HubParticipant*> ConversationsHub::ParticipantsDown() const {
  auto participants = Participants();
  std::reverse(participants.begin(), participants.end());
  return participants;
}

bool ConversationsHub::ParticipantsWantEphemeralListen() const {
  for (HubParticipant* participant : Participants()) {
    if (participant->WantsEphemeralListen()) {
      return true;
    }
  }
  return false;
}

void ConversationsHub::NotifyMessagingBuilt() {
  WireMeshConnectivity();
  for (HubParticipant* participant : Participants()) {
    participant->OnMessagingBuilt(*this);
  }
}

void ConversationsHub::NotifyPolicyPrefs(const PolicyPrefs& prefs) {
  for (HubParticipant* participant : Participants()) {
    participant->OnPolicyPrefs(prefs);
  }
}

ConversationsHub::~ConversationsHub() {
  transfer_lane_.Shutdown(); // before any member a transfer job uses goes away
  Shutdown();
}

void ConversationsHub::WireRelayAuthSigner() {
  if (!identity_) {
    return;
  }
  clients_.SetSigner(
      [this](const std::vector<uint8_t>& sign_bytes) -> Roe<std::string> { return identity_->SignBytes(sign_bytes); });
}

void ConversationsHub::UpdateOrgBackendClients(const HubSettings& settings) {
  clients_.Apply({settings.relay, settings.directory, settings.registration});
  relay_ = clients_.Relay();
  push_devices_ = clients_.PushDevices();
  client_compat_ = clients_.ClientCompat();
  registration_ = clients_.Registration();
  blob_ = clients_.Blob();
  if (account_locker_) {
    account_locker_->SetClient(clients_.Locker());
  }
}

void ConversationsHub::InstallOrgBackendClients(const HubSettings& settings) {
  UpdateOrgBackendClients(settings);
}

Roe<void> ConversationsHub::StartMesh(const HubSettings& settings) {
  StartupPhase phase("ConversationsHub::StartMesh");
  if (shutdown_requested_.load(std::memory_order_acquire) || AppRuntime::IsShuttingDown()) {
    return Error("shutdown in progress");
  }
  StopMesh();
  mesh_last_error_.clear();

  if (!mesh_owner_) {
    return Error("no mesh owner");
  }

  HubMeshOwner::StartRequest request;
  request.mesh = settings.mesh;
  request.identity = identity_.get();
  request.profile_data_dir = data_dir_;
  request.address_disclosure = &address_disclosure_;
  request.stopping = shutdown_requested_.load(std::memory_order_acquire);
  // Runs on the mesh's io lane (ReachabilityEngine): the hub's work is ours — hop to UI.
  request.on_reachability_updated = [this]() {
    AppRuntime::PostUI([this]() {
      if (shutdown_requested_.load(std::memory_order_acquire)) {
        return;
      }
      ApplyMeshAdmissionPolicies();
      PublishNodeAdvertisedAddrs();
      RegisterContactEndpoints();
      if (mesh_owner_) {
        mesh_owner_->RefreshMeshView(); // the local view follows the probe; seed reachability gates the hop policy
      }
      NoteObservedAddress();
      if (on_reachability_updated_) {
        on_reachability_updated_();
      }
    });
  };
  request.on_udp_socket_check = [this](const mesh::UdpSocketCheck& check) { ReportUdpSocketCheck(log(), check); };
  auto started = mesh_owner_->Start(std::move(request));
  if (!started) {
    mesh_last_error_ = started.error().message;
    return started.error();
  }
  mesh_ = *started;
  if (shutdown_requested_.load(std::memory_order_acquire)) {
    mesh_ = nullptr;
    mesh_owner_->Stop();
    mesh_owner_->Release();
    return Error("shutdown in progress");
  }
  if (mesh_->Amp()) {
    log().info << "amp stack listen=" << mesh_->AmpListenMultiaddr();
  } else {
    log().info << "mesh disabled (mesh_enabled=false); peer mesh underlay off";
  }
  StartMeshServices();
  ConfigureMeshQueries();
  if (mesh_owner_) {
    mesh_owner_->RefreshDirectory();
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

  if (mesh_owner_) {
    mesh_owner_->StartLanDiscovery([this](const mesh::LanMdnsDiscoveredPeer& peer) { OnLanMdnsPeerDiscovered(peer); });
  }

  ApplyMeshAdmissionPolicies();
  for (HubParticipant* participant : ParticipantsDown()) {
    participant->OnMeshMediaDetach();
  }
  if (mesh_owner_) {
    mesh_owner_->WireMeshObjects();
  }
  for (HubParticipant* participant : Participants()) {
    participant->OnMeshServicesStarted(*this);
  }
  PublishNodeAdvertisedAddrs();
  SyncLanMdnsAdvertisement();
  StartNetworkWatch();
}

void ConversationsHub::NoteObservedAddress() {
  if (!mesh_) {
    return;
  }
  const std::string observed = mesh_->Reachability().Snapshot().signals.dial_back_observed;
  if (observed.empty()) {
    return;
  }
  // Our public mapping moved (a NAT rebind, or a network change's re-probe): mobility churn (k6).
  if (!last_observed_addr_.empty() && observed != last_observed_addr_) {
    for (HubParticipant* participant : Participants()) {
      participant->OnObservedAddressChanged();
    }
  }
  last_observed_addr_ = observed;
}

void ConversationsHub::StartNetworkWatch() {
  if (watching_network_ || !mesh_owner_) {
    return;
  }
  watching_network_ = true;
  // Monitor thread → straight to the mesh / calls entry points (both any-thread). StopMesh stops the
  // watch before it tears the mesh down, so mesh_ outlives every callback.
  mesh_owner_->WatchNetwork([this](const NetworkChange& change) { OnLocalNetworkChanged(change); });
}

void ConversationsHub::StopNetworkWatch() {
  if (watching_network_ && mesh_owner_) {
    mesh_owner_->StopWatchingNetwork();
  }
  watching_network_ = false;
}

void ConversationsHub::OnLocalNetworkChanged(const NetworkChange& change) {
  const auto local = ReactMeshToNetworkChange(change, mesh_);
  for (HubParticipant* participant : Participants()) {
    participant->OnLocalNetworkChanged(change, local ? &*local : nullptr);
  }
  {
    // A tunnel with nothing under it (airplane mode, VPN still on) counts as offline for the status line.
    const bool linked = change.current.online && !change.current.tunnel_only;
    const int online = linked ? 1 : 0;
    if (network_online_.exchange(online) != online) {
      OnLinkNetwork(linked);
    }
  }
  // Offline or an unknown link keeps the last known kind of network.
  if (const AutoDownloadNetwork network = AutoDownloadNetworkOf(change.current, host_.mobile);
      network != AutoDownloadNetwork::Unknown) {
    attachment_network_.store(network);
    AppRuntime::PostUI([this]() { FeedAttachmentNetwork(); }); // where the policy is applied
  }
}

void ConversationsHub::PublishNodeAdvertisedAddrs() {
  // A017: Identify/Host advertisement retired. Amp peers learn via mDNS / pasted ADP MAs.
}

void ConversationsHub::PublishMobileCallScopedAddrs() {
  // A017: TCP ephemeral Identify publish retired; Amp listen is always advertised via mDNS.
}

void ConversationsHub::SyncMobileEphemeralListen() {
  if (!host_.mobile || !messaging_ready_) {
    return;
  }
  // D10/A017: Amp UDP accept is always on — TCP ephemeral listen retired. Refresh mDNS for LAN.
  if (mesh_ && mesh_->Amp()) {
    SyncLanMdnsAdvertisement();
  }
}

void ConversationsHub::SyncLanMdnsAdvertisement() {
  if (!mesh_owner_) {
    return;
  }

  const bool amp_up = mesh_ && mesh_->Amp() && !mesh_->AmpListenMultiaddr().empty();
  if (!amp_up) {
    return;
  }

  const bool node_listen = IsHelpNetworkEnabled();
  const bool ephemeral = host_.mobile && ParticipantsWantEphemeralListen();
  if (!node_listen && !ephemeral) {
    mesh_owner_->AdvertiseOnLan({}, 0);
    return;
  }

  const std::string peer_id = mesh_->Amp()->LocalPeerId();
  int amp_udp = 0;
  if (auto port = mesh::UdpPortFromAdpMultiaddr(mesh_->AmpListenMultiaddr())) {
    amp_udp = static_cast<int>(*port);
  }
  if (peer_id.empty() || amp_udp <= 0) {
    mesh_owner_->AdvertiseOnLan({}, 0);
    return;
  }
  mesh_owner_->AdvertiseOnLan(peer_id, amp_udp);
}

void ConversationsHub::OnLanMdnsPeerDiscovered(const mesh::LanMdnsDiscoveredPeer& peer) {
  // Discovery thread → the hub's thread (contacts, endpoints and the mDNS id set are ours).
  AppRuntime::PostUI([this, peer]() {
    if (peer.peer_id_base58.empty()) {
      return;
    }
    if (lan_mdns_contact_peer_ids_.find(peer.peer_id_base58) == lan_mdns_contact_peer_ids_.end()) {
      return;
    }
    const auto adp = mesh::LanMdnsDiscovery::BuildAdpMultiaddr(peer);
    if (!adp || !mesh_messaging_) {
      return;
    }
    if (mesh::IsLikelyUndialableLanIpv4(mesh::IpHostFromMultiaddrPrefix(*adp))) {
      log().info << "LAN mDNS skip undialable peer=" << peer.peer_id_base58 << " ma=" << *adp;
      return;
    }
    log().info << "LAN mDNS discovered peer=" << peer.peer_id_base58 << " ma=" << *adp;
    mesh_messaging_->RegisterPeerDirectEndpoint(peer.peer_id_base58, *adp);
    if (contacts_) {
      if (auto listed = contacts_->List(); listed) {
        for (const Contact& contact : *listed) {
          if (PeerIdFromContact(contact) != peer.peer_id_base58) {
            continue;
          }
          const DirectChatTarget target = DirectChatTargetFromContact(contact, ThreadChannel::E2ePublic);
          if (target.peer_identity_value.empty() || target.peer_identity_value == peer.peer_id_base58) {
            continue;
          }
          mesh_messaging_->RegisterPeerDirectEndpoint(target.peer_identity_value, *adp);
          log().info << "LAN mDNS dial alias peer=" << peer.peer_id_base58
                     << " dial_key=" << target.peer_identity_value;
        }
      }
    }
  });
}

mesh::DirectAudience ConversationsHub::EffectiveDirectAudience() const {
  // P004: a Node is reachable by role — its address is published anyway.
  return IsHelpNetworkEnabled() ? mesh::DirectAudience::Everyone : config_.mesh.direct_connections;
}

void ConversationsHub::PublishAddressDisclosure() {
  const mesh::DirectAudience audience = EffectiveDirectAudience();
  std::vector<Contact> book;
  if (contacts_) {
    if (auto listed = contacts_->List()) {
      book = std::move(*listed);
    }
  }
  // Our own account's other devices (M021): reachable as a Friendly contact is.
  std::vector<std::string> own_devices;
  if (account_keys_) {
    if (auto identity = Identity().Get(); identity && !identity->account_id.empty()) {
      if (auto kept = account_keys_->Kept(identity->account_id); kept && *kept) {
        own_devices = (*kept)->devices;
      }
    }
  }
  address_disclosure_.Publish(BuildAddressDisclosurePolicy(audience, book, own_devices));
}

void ConversationsHub::ApplyMeshAdmissionPolicies() {
  PublishAddressDisclosure();
  const bool prefer = config_.mesh.prefer_contacts_for_routing;
  const bool node = IsHelpNetworkEnabled();
  // A017: Amp listen is always on; treat mobile in-call as link-scope hosting.
  const bool mobile_ephemeral = host_.mobile && ParticipantsWantEphemeralListen();
  std::unordered_set<std::string> contact_ids;
  if (contacts_) {
    if (auto listed = contacts_->List()) {
      for (const std::string& id : ContactPeerIds(*listed)) {
        contact_ids.insert(id);
      }
    }
  }

  mesh::MeshReachabilityClass reach_class = mesh::MeshReachabilityClass::Unknown;
  switch (Reachability().status) {
    case mesh::ReachabilityStatus::Reachable:
      reach_class = mesh::MeshReachabilityClass::Reachable;
      break;
    case mesh::ReachabilityStatus::OutboundOnly:
      reach_class = mesh::MeshReachabilityClass::OutboundOnly;
      break;
    case mesh::ReachabilityStatus::Blocked:
      reach_class = mesh::MeshReachabilityClass::Blocked;
      break;
    case mesh::ReachabilityStatus::Unknown:
    case mesh::ReachabilityStatus::Checking:
    default:
      reach_class = mesh::MeshReachabilityClass::Unknown;
      break;
  }

  mesh::RelayScopeMask serve_mask = ProviderServeScopeMask(reach_class, node);
  const bool force_limit_strangers = node && (reach_class == mesh::MeshReachabilityClass::OutboundOnly ||
                                              reach_class == mesh::MeshReachabilityClass::Blocked);
  const bool limit_strangers = (prefer && node && !contact_ids.empty()) || force_limit_strangers;
  if (!limit_strangers && node) {
    serve_mask |= static_cast<mesh::RelayScopeMask>(mesh::RelayScope::Public);
  }

  if (mesh_ && mesh_->AmpCircuitServer()) {
    mesh::CircuitRelayAdmissionPolicy policy;
    policy.prefer_contacts_only = limit_strangers;
    policy.serve_scope_mask = serve_mask;
    policy.contact_peer_ids = contact_ids;
    mesh_->AmpCircuitServer()->SetAdmissionPolicy(std::move(policy));
  }
  if (mesh_ && mesh_->AmpMediaRelayServer()) {
    mesh::MediaRelayAdmissionPolicy policy;
    if (mobile_ephemeral) {
      policy.prefer_contacts_only = true;
      policy.serve_scope_mask = mesh::kRelayScopeLinkSiteSocial;
      policy.contact_peer_ids = contact_ids;
    } else {
      policy.prefer_contacts_only = limit_strangers;
      policy.serve_scope_mask = serve_mask;
      policy.contact_peer_ids = contact_ids;
    }
    mesh_->AmpMediaRelayServer()->SetAdmissionPolicy(std::move(policy));
  }
}

void ConversationsHub::PublishMeshConfig() {
  // As it applies on this device (a phone is a client): what the mesh's readers act on.
  auto snapshot = std::make_shared<const MeshConfig>(EffectiveMeshConfig(config_.mesh, host_.mobile));
  {
    std::lock_guard lock(mesh_config_mu_);
    mesh_config_snapshot_ = std::move(snapshot);
  }
  PublishAddressDisclosure();
}

std::shared_ptr<const MeshConfig> ConversationsHub::MeshConfigSnapshot() const {
  std::lock_guard lock(mesh_config_mu_);
  return mesh_config_snapshot_;
}

void ConversationsHub::SyncStayReachable() {
  const bool want = config_.mesh.reachable_without_relay && host_.desktop && mesh_ && mesh_ready_ &&
                    !shutdown_requested_.load(std::memory_order_acquire);
  if (mesh_owner_) {
    mesh_owner_->StayReachable(want);
  }
}

void ConversationsHub::StopMesh() {
  if (mesh_owner_) {
    mesh_owner_->StayReachable(false);
  }
  StopNetworkWatch();
  mobile_ephemeral_start_inflight_ = false;
  mobile_ephemeral_start_inflight_at_ms_ = 0;
  mobile_ephemeral_stop_inflight_ = false;
  mobile_ephemeral_last_start_error_.clear();
  auto detach_transports = [this]() {
    if (mesh_owner_) {
      mesh_owner_->StopLanDiscovery();
    }
    // Amp chat/announce/broadcast transports hold PeerLinks& — detach before MeshHost::Stop
    // destroys Amp (otherwise ~AmpBroadcastTransport::Stop UAFs).
    if (mesh_messaging_) {
      mesh_messaging_->DetachAmpTransports();
    }
  };
  // Order: connectivity async ops dropped → call-media teardown bracketed by circuit aborts (calls
  // let go of the mesh objects) → relay client closed → Amp transports detached →
  // MeshHost::Stop (media_relay, circuit, dial-back, runtime; joins workers) → call-media freed →
  // dial registry / circuit reach freed.
  mesh::MeshHost* mesh = mesh_;
  if (mesh_owner_) {
    mesh_owner_->BeginMeshStop();
  }
  const std::function<void()> abort_circuits = [mesh]() {
    if (mesh) {
      mesh->AbortInflightCircuitRequests();
    }
  };
  for (HubParticipant* participant : ParticipantsDown()) {
    participant->OnMeshStopping(abort_circuits);
  }
  if (mesh_owner_) {
    mesh_owner_->DropMediaRelayClient();
  }
  detach_transports();
  if (mesh && mesh_owner_) {
    mesh_owner_->Stop();
  }
  for (HubParticipant* participant : ParticipantsDown()) {
    participant->OnMeshStopped();
  }
  if (mesh_owner_) {
    mesh_owner_->EndMeshStop();
  }
  mesh_ = nullptr;
  if (mesh && mesh_owner_) {
    mesh_owner_->Release();
  }
}

void ConversationsHub::AbortParticipantsForShutdown() {
  for (HubParticipant* participant : ParticipantsDown()) {
    participant->OnShutdownRequested();
  }
}

void ConversationsHub::RequestShutdown() {
  shutdown_requested_.store(true, std::memory_order_release);
  // Unblock circuit / dial waits inside an in-flight StartMesh / EnsureMessagingReady.
  AbortParticipantsForShutdown();
  if (mesh_) {
    mesh_->SetStopping(true);
    mesh_->AbortInflightCircuitRequests();
  }
}

void ConversationsHub::CancelShutdownRequest() {
  shutdown_requested_.store(false, std::memory_order_release);
  if (mesh_) {
    mesh_->SetStopping(false);
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
    mesh_messaging_->SetOnBriefRelayHealthChanged(nullptr);
    mesh_messaging_->SetOnMessagesChanged(nullptr);
    mesh_messaging_->SetOnDeliveryNotice(nullptr);
    mesh_messaging_->SetOnBackgroundUnread(nullptr);
    mesh_messaging_->SetGroupMembership(nullptr);
    mesh_messaging_->SetOnSiblingNotice(nullptr);
    mesh_messaging_->OwnAccountCards().SetOnDeviceJoined(nullptr);
    mesh_messaging_->SetAttachmentDownloads(nullptr);
  }
  if (inbox_) {
    inbox_->SetGroupMembership(nullptr);
    inbox_->SetAttachmentDownloads(nullptr);
  }
  system_controls_.Unbind(kGroupRosterSyncFamily);
  system_controls_.Unbind(kCopyAskFamily);
  system_controls_.Unbind(kCardAskFamily);
  system_controls_.Unbind(kGroupLinkJoinFamily);
  account_locker_.reset();
  group_links_.reset();
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
  for (HubParticipant* participant : ParticipantsDown()) {
    participant->OnMessagingDropped();
  }
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
    const std::vector<std::string> peer_ids = PeerIdsFromContact(contact);
    for (const std::string& peer_id : peer_ids) {
      lan_mdns_contact_peer_ids_.insert(peer_id);
    }
  }
  ApplyMeshAdmissionPolicies();
}

std::vector<mesh::MeshDirectoryNode> ConversationsHub::DirectoryNodes() const {
  return mesh_owner_ ? DirectoryNodesForDial(mesh_owner_->DirectoryNodesSnapshot())
                     : std::vector<mesh::MeshDirectoryNode>{};
}

void ConversationsHub::RegisterMeshDirectoryEndpoints() {
  if (!mesh_messaging_ || !mesh_owner_ || !mesh_owner_->HasDirectoryCache()) {
    return;
  }
  const mesh::AmpDialLocalContext local_ctx = mesh::CollectAmpDialLocalContext();
  for (const mesh::MeshDirectoryNode& node : DirectoryNodes()) {
    if (node.peer_id.empty() || node.multiaddrs.empty()) {
      continue;
    }
    mesh_messaging_->RegisterPeerDirectEndpoints(node.peer_id, mesh::RankAmpDialMultiaddrs(node.multiaddrs, local_ctx));
  }
}

void ConversationsHub::RegisterDhtBootstrapEndpoints() {
  if (!mesh_messaging_) {
    return;
  }
  MeshConfig mesh_cfg = config_.mesh;
  std::vector<mesh::MeshDirectoryNode> directory_nodes;
  directory_nodes = DirectoryNodes();
  for (const std::string& ma : EffectiveBootstrapPeers(mesh_cfg, directory_nodes)) {
    const std::string peer_id = PeerIdFromMultiaddr(ma);
    if (peer_id.empty() || ma.empty()) {
      continue;
    }
    mesh_messaging_->RegisterPeerDirectEndpoint(peer_id, ma);
  }
  RegisterMeshDirectoryEndpoints();
}

void ConversationsHub::ApplyDhtFindPeerResult(const std::string& peer_id, const mesh::PeerRoutingRecord& record) {
  if (!mesh_messaging_ || peer_id.empty()) {
    return;
  }
  mesh_messaging_->RegisterPeerDirectEndpoints(
      peer_id, mesh::RankAmpDialMultiaddrs(record.multiaddrs, mesh::CollectAmpDialLocalContext()));
}

void ConversationsHub::ConfigureMeshQueries() {
  if (!mesh_ || !mesh_->Amp() || !identity_ || !mesh_owner_) {
    return;
  }
  RegisterDhtBootstrapEndpoints();
  HubMeshOwner::QueryInputs inputs;
  inputs.mesh = config_.mesh;
  inputs.directory = config_.directory;
  inputs.identity = identity_.get();
  inputs.directory_nodes = [this]() { return DirectoryNodes(); };
  // The mesh (and its DHT) goes before the owner's connectivity.
  inputs.reach_held_down = [connectivity = &mesh_owner_->Connectivity()](const std::string& peer_id) {
    return connectivity && mesh::ReachHeldDown(connectivity->ReachFacts(peer_id), mesh::PeerReachRecord::NowMs());
  };
  constexpr size_t kDirectoryPeersPerList = 2;
  inputs.choose_directory_peers = [connectivity = &mesh_owner_->Connectivity()](const std::vector<std::string>& keys) {
    return connectivity ? connectivity->ChoosePeers(keys, kDirectoryPeersPerList) : keys;
  };
  mesh_owner_->ConfigureQueries(inputs);
}

bool ConversationsHub::IsContactReachable(const Contact& contact) const {
  bool keys_known = false;
  if (account_keys_) {
    if (auto account = ContactAccountId(contact)) {
      keys_known = static_cast<bool>(
          account_keys_->KeptSigning().Resolve(ContactIdKindToString(mesh::ContactIdKind::Account), *account));
    }
  }
  return IsContactReachableForMessaging(contact, relay_ != nullptr, keys_known);
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
    if (auto hit = contacts_->FindByIdentity(identity, mesh::ContactIdKind::Account)) {
      if (hit->has_value()) {
        peer_id = PeerIdFromContact(**hit);
      }
    }
    if (peer_id.empty()) {
      if (auto hit = contacts_->FindByIdentity(identity, mesh::ContactIdKind::RelayUser)) {
        if (hit->has_value()) {
          peer_id = PeerIdFromContact(**hit);
        }
      }
    }
    if (peer_id.empty()) {
      if (auto hit = contacts_->FindByIdentity(identity, mesh::ContactIdKind::PeerId)) {
        if (hit->has_value()) {
          peer_id = PeerIdFromContact(**hit);
        }
      }
    }
  }
  if (peer_id.empty()) {
    peer_id = identity;
  }
  // Same Amp warm path as open-chat WarmPeerForThread — call invite/accept should share it.
  mesh_messaging_->WarmPeerByKey(peer_id);
  if (mesh_ && mesh_->AmpDht() && IsHelpNetworkEnabled() && config_.mesh.capabilities.dht) {
    mesh_->AmpDht()->FindPeer(peer_id, [this, peer_id](mesh::AmpDhtProtocol::FindPeerRoe result) {
      if (!result) {
        return;
      }
      ApplyDhtFindPeerResult(peer_id, result->record);
    });
  }
}

Roe<void> ConversationsHub::Initialize(const HubSettings& settings, const std::string& profile_data_dir) {
  if (initialized_) {
    return {};
  }
  // Profile reset re-initializes the same hub after Shutdown — without this StartMesh refused
  // forever with "shutdown in progress".
  shutdown_requested_.store(false, std::memory_order_release);

  config_ = settings;
  policy_ = settings.policy;
  PublishMeshConfig();
  data_dir_ = profile_data_dir;
  std::error_code ec;
  std::filesystem::create_directories(data_dir_, ec);

  profile_id_ = std::filesystem::path(data_dir_).filename().string();
  if (profile_id_.empty()) {
    profile_id_ = "default";
  }

  if (!open_stores_ || !open_content_index_) {
    return Error("No storage for the profile's stores");
  }
  auto opened_stores = open_stores_(data_dir_, profile_id_);
  if (!opened_stores) {
    return opened_stores.error();
  }
  store_ = std::move(opened_stores->threads);
  psk_store_ = std::move(opened_stores->peer_sessions);
  control_lane_seen_ = std::move(opened_stores->control_lane_seen);
  group_roster_ = std::move(opened_stores->rosters);
  invite_links_ = std::move(opened_stores->invite_links);
  thread_prefs_ = std::move(opened_stores->thread_prefs);
  thread_mentions_ = std::move(opened_stores->thread_mentions);
  peer_card_rows_ = std::move(opened_stores->peer_cards);
  content_ = std::make_shared<CasStore>(data_dir_, profile_id_, open_content_index_(data_dir_));
  at_rest_ = std::move(opened_stores->at_rest);
  profile_db_path_ = std::move(opened_stores->profile_db_path);
  message_store_bytes_ = std::move(opened_stores->message_store_bytes);
  contacts_ = std::make_unique<ContactsStore>(data_dir_);
  // Contacts decide who may learn our address: republish on every change (any thread → UI).
  contacts_->SetOnChanged([this]() {
    AppRuntime::PostUI([this]() {
      if (!shutdown_requested_.load(std::memory_order_acquire)) {
        PublishAddressDisclosure();
      }
    });
    PublishAccountLockerSoon();
  });
  PublishAddressDisclosure();
  identity_ = std::make_unique<IdentityStore>(data_dir_, profile_id_);
  initiation_billing_ = std::make_unique<InitiationBillingStore>(data_dir_);
  (void)initiation_billing_->Load();
  payment_promises_ = std::make_unique<PaymentPromiseStore>(data_dir_);
  (void)payment_promises_->Load();

  {
    StartupPhase phase("ConversationsHub::ReconcileOutbox");
    // Also the first open of profile.db: an unusable chat store shows here, not only at its first use.
    if (auto reconciled = store_->ReconcileOutbox(); !reconciled) {
      log().warning << "Chat store: " << reconciled.error().message;
    }
  }

  InstallOrgBackendClients(settings);

  // The account locker keeps every group (G013): republish it on any roster change.
  group_roster_->SetOnChanged([this](const GroupRosterChange& change) {
    PublishAccountLockerSoon();
    if (mesh_messaging_) {
      mesh_messaging_->DecideHeldMentions(change.group_id); // G015: a check that settled decides its held mentions
    }
    for (HubParticipant* participant : Participants()) {
      participant->OnGroupRosterChanged(change);
    }
  });
  for (HubParticipant* participant : Participants()) {
    if (auto opened = participant->OnStoresOpened(*this); !opened) {
      return opened.error();
    }
  }
  group_invite_gate_ = std::make_unique<GroupInviteGate>(InviterStandingFromContacts(*contacts_), *group_roster_);
  group_invite_gate_->SetInLiveCall(in_live_call_);
  directory_shadows_ = std::make_unique<DirectoryShadowCache>(directory_);
  peer_labels_ = std::make_unique<PeerDisplayResolver>(
      *contacts_, PeerDisplaySourcesFrom(*directory_shadows_, group_roster_.get()));
  inbox_ = std::make_unique<InboxController>(*store_, *contacts_, *peer_labels_, directory_shadows_.get());
  inbox_->SetSystemControls(&system_controls_);
  inbox_->SetThreadMentions(thread_mentions_.get());
  directory_shadows_->SetOnUpdated([this]() {
    if (inbox_) {
      inbox_->NotifyThreadChanged();
    }
  });
  directory_shadows_->SetOnHitCached([this](const mesh::DirectoryHit& hit) {
    EnsureDirectoryHitIconCached(hit);
    // A contact added from a bare id (a stranger who wrote first) gets the directory's nickname as soon as
    // it is known, so the list shows a name instead of the id. The user's own remark is never touched.
    if (contacts_ && !hit.nickname.empty()) {
      if (auto account = PrimaryAccountIdFromHit(hit)) {
        if (auto contact = contacts_->FindByIdentity(*account, mesh::ContactIdKind::Account);
            contact && *contact && (*contact)->remote.nickname.empty()) {
          (void)contacts_->ApplyRemoteSnapshot((*contact)->id, hit, 0);
        }
      }
    }
    if (!initiation_billing_) {
      return;
    }
    std::string person_id;
    if (auto account = PrimaryAccountIdFromHit(hit)) {
      person_id = *account;
    } else {
      for (const mesh::ContactId& id : hit.ids) {
        if (id.kind == mesh::ContactIdKind::RelayUser && !id.value.empty()) {
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

  if (directory_.Configured() && mesh_owner_) {
    // The directory's mesh nodes, kept by the mesh's owner. An update lands where the refresh ran (a worker):
    // the hub's work is ours — hop to UI.
    mesh_owner_->StartDirectoryCache(directory_, data_dir_, [this]() {
      AppRuntime::PostUI([this]() {
        if (shutdown_requested_.load(std::memory_order_acquire)) {
          return;
        }
        RegisterMeshDirectoryEndpoints();
        ConfigureMeshQueries();
        mesh_owner_->PushHopPolicySoon(); // directory nodes are rendezvous / seed candidates
      });
    });
  }

  if (secrets_ != nullptr) {
    secrets_->RegisterDekConsumer(identity_.get());
    for (IDekConsumer* at_rest : at_rest_) {
      secrets_->RegisterDekConsumer(at_rest);
    }
    for (HubParticipant* participant : Participants()) {
      for (IDekConsumer* consumer : participant->DekConsumers()) {
        secrets_->RegisterDekConsumer(consumer);
      }
    }
  }
  BindPricingFamilies();
  peer_cards_ = std::make_unique<PeerCards>(*peer_card_rows_);
  // The account locker keeps how new each account's newest card is (a restored device refuses older ones).
  peer_cards_->SetOnNewerCard([this]() { PublishAccountLockerSoon(); });
  account_keys_ = std::make_unique<AccountKeyResolver>(signing_key_store_, kem_key_store_, *peer_cards_, directory_);
  account_keys_->LoadKeptCards();
  contact_cards_ = std::make_unique<ContactCardExchange>(*contacts_, *account_keys_);
  contact_profile_refresh_ = std::make_unique<ContactProfileRefresh>(*contacts_, directory_);
  contact_cards_->SetCardFetcher(
      [this](const std::string& peer_id, const std::string& account_id, std::function<void(Roe<PeerCardRecord>)> done) {
        // On the hub's thread, with the address book as it is now: a contact just added (a scanned QR code) is
        // one we may dial — the republish its add posted may not have run yet (#409).
        AppRuntime::PostUI([this, peer_id, account_id, done = std::move(done)]() mutable {
          if (!mesh_messaging_ || shutdown_requested_.load(std::memory_order_acquire)) {
            done(Error("Messaging not ready"));
            return;
          }
          PublishAddressDisclosure();
          mesh_messaging_->AccountCards().Fetch(peer_id, account_id, std::move(done));
        });
      });

  // The messaging stack, relay-capable once the profile unlocks (no Amp until the mesh is up).
  BuildMessagingObjects();

  messaging_ready_ = false;
  if (lifecycle_background_id_ == 0) {
    lifecycle_background_id_ = AppLifecycle::AddBackgroundListener([this]() { OnAppBackground(); });
    lifecycle_foreground_id_ = AppLifecycle::AddForegroundListener([this]() { OnAppForeground(); });
  }
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

void ConversationsHub::BuildMessagingObjects() {
  mesh_messaging_ =
      std::make_unique<MeshDeliveryOrchestrator>(*store_, *contacts_, *identity_, relay_, *inbox_, *account_keys_,
                                                 *psk_store_, *group_roster_, group_invite_gate_.get());
  mesh_messaging_->SetSystemControls(&system_controls_);
  // The relay's poll results feed the status line (#175); a fresh orchestrator starts with no result.
  mesh_messaging_->SetOnBriefRelayHealthChanged(
      [this](const MeshDeliveryOrchestrator::BriefRelayHealthState health) { OnBriefRelayHealthChanged(health); });
  {
    std::unique_lock lock(link_mu_);
    ResetLinkHealthLocked();
    ArmLinkGraceLocked(); // messaging is up: the 5 s clock for the first poll result starts now
  }
  mesh_messaging_->SetThreadMentions(thread_mentions_.get());
  // Our device list changed: our address goes to our other devices too (M021).
  mesh_messaging_->SetOnOwnDevicesChanged([this]() { PublishAddressDisclosure(); });
  // A device with no link up is reached through the seed it is parked on (on the hub's thread).
  mesh_messaging_->AccountCards().SetReach([this](const std::string& peer_id, std::function<void(bool)> done) {
    AppRuntime::PostUI([this, peer_id, done = std::move(done)]() {
      if (!mesh_ || !mesh_ready_) {
        done(false);
        return;
      }
      if (!mesh_owner_) {
        done(false);
        return;
      }
      mesh_owner_->Connectivity().TryEnsurePeerReachableAsync(
          peer_id, [done](mesh::CircuitReachRoe reached) { done(static_cast<bool>(reached)); });
    });
  });
  mesh_messaging_->SetProfileDataDir(data_dir_);
  mesh_messaging_->SetContent(content_);
  mesh_messaging_->SetControlLaneSeen(control_lane_seen_.get());
  mesh_messaging_->SetAddressDisclosure(&address_disclosure_);
  mesh_messaging_->SetInitiationBillingStore(initiation_billing_.get());
  mesh_messaging_->SetPeerRouteSources(directory_shadows_.get(), &directory_);
  WireAttachmentDownloads();
  CleanStaleAttachmentFiles(); // once per start: staged sends older than a day, half-finished downloads
  group_membership_ = std::make_unique<GroupMembershipWorkflow>(*store_, *contacts_, *identity_, *group_roster_,
                                                                *group_invite_gate_, *mesh_messaging_, *account_keys_);
  inbox_->SetGroupMembership(group_membership_.get());
  mesh_messaging_->SetGroupMembership(group_membership_.get());
  BindGroupRosterSyncFamily();
  BindCopyAskFamily();
  BindCardAskFamily();
  group_links_ = std::make_unique<GroupInviteLinkWorkflow>(*group_roster_, invite_links_.get(), *group_membership_,
                                                           *mesh_messaging_, *identity_);
  BindGroupLinkJoinFamily();
  account_locker_ = std::make_unique<AccountLockerWorkflow>(
      *identity_, *contacts_,
      AccountLockerWorkflow::Sources{
          .groups = [this]() -> Roe<std::vector<AccountStateGroup>> {
            return group_membership_ ? group_membership_->StateGroups()
                                     : Roe<std::vector<AccountStateGroup>>(std::vector<AccountStateGroup>{});
          },
          .card_floors = [this]() -> Roe<std::vector<PeerCardFloor>> {
            return peer_cards_ ? peer_cards_->NewestKnown()
                               : Roe<std::vector<PeerCardFloor>>(std::vector<PeerCardFloor>{});
          },
          .take_dissolved =
              [this](const std::vector<AccountStateGroup>& groups) {
                if (group_membership_) {
                  for (const AccountStateGroup& group : groups) {
                    group_membership_->EndDissolvedFromLocker(group);
                  }
                }
              }});
  account_locker_->SetClient(clients_.Locker());
  account_locker_->SetDataDir(data_dir_);
  // The account's devices keep one address book: a write here nudges the others; a nudge from one syncs here.
  account_locker_->SetOnWritten([this]() {
    if (mesh_messaging_) {
      mesh_messaging_->NudgeSiblings();
    }
  });
  mesh_messaging_->SetOnSiblingNotice([this]() { PublishAccountLockerSoon(); });
  // A device just joined our list (we are the master): our contacts hear of it, so their next message reaches it.
  mesh_messaging_->OwnAccountCards().SetOnDeviceJoined(
      [this](const AccountCard&) { AppRuntime::PostWorkerNormal([this]() { AnnounceOwnDevicesToContacts(); }); });
  NotifyMessagingBuilt();
  actions_ =
      std::make_unique<ContactActionDispatcher>(*inbox_, *contacts_, *identity_, *store_, group_membership_.get(),
                                                registration_, mesh_messaging_.get(), &directory_);
  group_invite_gate_->SetInboundPolicy(policy_.group_invite_policy);
  group_membership_->SetInboundPolicy(policy_.group_invite_policy);
  NotifyPolicyPrefs(policy_);
}

Roe<void> ConversationsHub::BuildLocalMessagingStack() {
  if (shutdown_requested_.load(std::memory_order_acquire)) {
    return Error("shutdown in progress");
  }
  WireRelayAuthSigner();
  // Initialize built the stack; keep it — workers and timers may already hold it. Build again only
  // after a discarded bring-up dropped it.
  if (!mesh_messaging_) {
    BuildMessagingObjects();
  }
  RegisterContactEndpoints();
  // The router exists with messaging, agent or not: a send to a person needs no agent.
  if (!router_) {
    router_ = MakeRouter();
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
  if (!mesh_messaging_) {
    return Error("local messaging stack not built");
  }

  mesh::IChatPeerLinks* amp_links = nullptr;
  std::function<void()> amp_pump;
  std::function<void(std::function<void()>)> amp_worker;
  std::function<void(std::function<void()>)> amp_post_io;
  std::function<void(std::chrono::milliseconds, std::function<void()>)> amp_post_after;
  if (auto chat = mesh_->ChatDeps(); chat) {
    amp_links = &chat->links;
    amp_pump = std::move(chat->io.io_pump);
    amp_worker = std::move(chat->io.post_worker);
    amp_post_io = std::move(chat->io.post_io);
    amp_post_after = std::move(chat->io.post_after);
  }
  if (amp_pump && !amp_worker) {
    amp_worker = [](std::function<void()> task) { AppRuntime::PostWorkerNormal(std::move(task)); };
  }
  if (!amp_links) {
    log().warning << "AttachAmpMessagingStack: Amp chat deps unavailable";
    return {};
  }

  // A peer quiet for kRecentPeerWarmMs loses chat's / the call's warm mark (#577): the chat links port can mark,
  // not clear, so the clear goes to Amp, as MeshPorts' MarkWarm does.
  mesh_messaging_->SetClearPeerWarm([this](const std::string& peer_key) {
    if (pp::amp::AmpStack* amp = mesh_ ? mesh_->Amp() : nullptr) {
      amp->Runtime().WithIoLock([&] { amp->Links().ClearWarm(peer_key); });
    }
  });
  // Keep the same MeshDeliveryOrchestrator instance — timers / SyncInbox workers may already
  // hold `this`. Recreating here caused "mutex lock failed: Invalid argument" + segfault.
  mesh_messaging_->AttachAmpTransports(amp_links, std::move(amp_pump), std::move(amp_worker), std::move(amp_post_io),
                                       std::move(amp_post_after));
  WireAttachmentDownloads();
  // Same orchestrator, now with Amp transports: participants keep what they built on it (rebuilding
  // the call sessions here abandoned a call already ringing over the relay).
  WireMeshConnectivity();
  RegisterContactEndpoints();
  RegisterMeshDirectoryEndpoints();
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
      SyncStayReachable();
    });
  });
}

Roe<void> ConversationsHub::EnsureMessagingReady() {
  StartupPhase phase("ConversationsHub::EnsureMessagingReady");
  if (shutdown_requested_.load(std::memory_order_acquire) || AppRuntime::IsShuttingDown()) {
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

  // Before the first poll: a restored device refuses cards older than the ones it knew.
  ApplyRestoredCardFloors();
  messaging_ready_ = true;
  mesh_ready_ = false;
  StartCoordinatorTimers();
  NotifyMessagingReady();
  ScheduleMeshBringUp();
  AppRuntime::PostWorkerNormal([this]() {
    ResumeGroupRosterChecks();
    AnnounceRestoredDevice();
  });
  RefreshStaleContactProfiles();
  return {};
}

void ConversationsHub::BindAgentInbound(AgentInboundPorts ports) {
  agent_inbound_ = std::move(ports);
  // In place: the router keeps its callbacks, and an unbound agent (teardown, profile reset) leaves
  // it no ports into the old agent session.
  if (router_) {
    router_->SetAgent(agent_inbound_);
  } else if (mesh_messaging_) {
    router_ = MakeRouter();
  }
}

std::unique_ptr<MessageRouter> ConversationsHub::MakeRouter() {
  auto router = std::make_unique<MessageRouter>(*inbox_, *mesh_messaging_, agent_inbound_, *store_);
  // "Don't ask again" for sharing with the AI is kept per chat in the thread prefs.
  router->SetSharedAiConfirmedPort([this](const std::string& thread_id) {
    if (!thread_prefs_) {
      return false;
    }
    auto prefs = thread_prefs_->Get(thread_id);
    return prefs && prefs->share_ai_confirmed;
  });
  return router;
}

void ConversationsHub::BindSecrets(ProfileSecretsEngine& secrets) {
  secrets_ = &secrets;
}

std::vector<GroupInviteGate::InviteInCall> ConversationsHub::GroupInvitesInCall(const std::string& call_id) const {
  if (!group_invite_gate_) {
    return {};
  }
  return group_invite_gate_->InvitesInCall(call_id, util::NowUnixMs());
}

std::string ConversationsHub::KeptCardNickname(const std::string& account_id) {
  if (!account_keys_) {
    return {};
  }
  auto kept = account_keys_->Kept(account_id);
  return kept && *kept ? (*kept)->nickname : std::string();
}

PeerSigningKeyStore& ConversationsHub::SigningKeys() {
  return signing_key_store_;
}

void ConversationsHub::TickMesh() {
  if (!messaging_ready_) {
    return;
  }
  // Mesh UDP drain is MeshHost MeshPumpThread (~5ms). Policy stays on the 1s timer.
  if (mesh_messaging_) {
    mesh_messaging_->TickMesh();
  }
  if (mesh_owner_) {
    mesh_owner_->MaybeRefreshDirectory();
  }
  if (mesh_ && mesh_->AmpDht()) {
    mesh_->AmpDht()->Tick();
  }
  SyncMobileEphemeralListen();
  SyncLanMdnsAdvertisement();
  TickReachabilityUx();
}

namespace {

constexpr auto kHubPolicyTimerInterval = std::chrono::seconds(1);

} // namespace

void ConversationsHub::StartCoordinatorTimers() {
  if (hub_policy_timer_id_ == 0) {
    hub_policy_timer_id_ = AppRuntime::ScheduleCoordinatorRepeating(kHubPolicyTimerInterval, [this]() { TickMesh(); });
  }
  if (locker_sync_timer_id_ == 0) {
    // What another device of the account changed (a contact removed, a remark) arrives through the locker.
    locker_sync_timer_id_ = AppRuntime::ScheduleCoordinatorRepeating(std::chrono::milliseconds(kLockerSyncIntervalMs),
                                                                     [this]() { PublishAccountLockerSoon(); });
  }
  // Relay poll must not depend on ChatController::WireMessagingBindings (can skip if UI
  // wiring races unlock). Own the sync handler here as soon as messaging is ready.
  BackgroundSyncScheduler::Instance().SetSyncHandler([this](bool force) {
    const bool call_wake = BackgroundSyncScheduler::Instance().ConsumeUrgentWake();
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
  if (locker_sync_timer_id_ != 0) {
    AppRuntime::CancelCoordinatorTimer(locker_sync_timer_id_);
    locker_sync_timer_id_ = 0;
  }
  if (hub_policy_timer_id_ != 0) {
    AppRuntime::CancelCoordinatorTimer(hub_policy_timer_id_);
    hub_policy_timer_id_ = 0;
  }
  BackgroundSyncScheduler::Instance().SetSyncHandler(nullptr);
}

mesh::ReachabilitySnapshot ConversationsHub::Reachability() const {
  return mesh_ ? mesh_->Reachability().Snapshot() : mesh::ReachabilitySnapshot{};
}

bool ConversationsHub::IsHelpNetworkEnabled() const {
  return MeshRoleOf(EffectiveMeshConfig(config_.mesh, host_.mobile)) == MeshRole::Node;
}

void ConversationsHub::SetOnReachabilityUpdated(std::function<void()> callback) {
  on_reachability_updated_ = std::move(callback);
}

void ConversationsHub::SetOnLinkStateChanged(std::function<void(LinkState)> callback) {
  std::lock_guard lock(link_mu_);
  on_link_state_changed_ = std::move(callback);
}

ConversationsHub::LinkState ConversationsHub::CurrentLinkState() const {
  std::lock_guard lock(link_mu_);
  return link_state_;
}

void ConversationsHub::SetLinkGraceScheduler(
    std::function<void(std::chrono::milliseconds, std::function<void()>)> scheduler) {
  std::lock_guard lock(link_mu_);
  link_grace_scheduler_ = std::move(scheduler);
}

void ConversationsHub::ResetLinkHealthLocked() {
  link_health_ = MeshDeliveryOrchestrator::BriefRelayHealthState::Unknown;
  if (mesh_messaging_) {
    // Its atomic still holds the old Ok: without this the next Ok poll is "no change", fires nothing, and the
    // line sticks on Connecting (iPhone, 2026-11-09).
    mesh_messaging_->ResetBriefRelayHealth();
  }
}

void ConversationsHub::OnLinkNetwork(const bool linked) {
  std::unique_lock lock(link_mu_);
  link_net_ = linked ? 1 : 0;
  if (!linked) {
    // An outage makes the last relay result stale: when the network returns, Connecting shows until a new one.
    ResetLinkHealthLocked();
  }
  ++link_grace_gen_; // cancels a pending timer either way
  link_grace_over_ = false;
  if (linked && link_state_ == LinkState::Offline) {
    // Red says "no network", which is false now: replace it at once, no grace (the line has nothing to keep).
    link_grace_over_ = true;
  } else if (linked) {
    ArmLinkGraceLocked();
  }
  ReevaluateLink(std::move(lock));
}

void ConversationsHub::OnBriefRelayHealthChanged(const MeshDeliveryOrchestrator::BriefRelayHealthState health) {
  std::unique_lock lock(link_mu_);
  link_health_ = health;
  if (health != MeshDeliveryOrchestrator::BriefRelayHealthState::Unknown) {
    ++link_grace_gen_; // a result is in: the Unknown timer is moot
    link_grace_over_ = false;
  }
  ReevaluateLink(std::move(lock));
}

void ConversationsHub::ArmLinkGraceLocked() {
  if (link_net_ != 1 || link_health_ != MeshDeliveryOrchestrator::BriefRelayHealthState::Unknown) {
    return;
  }
  const uint64_t gen = ++link_grace_gen_;
  link_grace_over_ = false;
  auto fire = link_grace_lifetime_.Bind([this, gen]() { OnLinkGraceElapsed(gen); });
  if (link_grace_scheduler_) {
    link_grace_scheduler_(kLinkUnknownGrace, std::move(fire));
  } else {
    AppRuntime::ScheduleCoordinatorOneShot(kLinkUnknownGrace, std::move(fire));
  }
}

void ConversationsHub::OnLinkGraceElapsed(const uint64_t gen) {
  std::unique_lock lock(link_mu_);
  if (gen != link_grace_gen_) {
    return;
  }
  link_grace_over_ = true;
  ReevaluateLink(std::move(lock));
}

void ConversationsHub::ReevaluateLink(std::unique_lock<std::mutex> lock) {
  using Health = MeshDeliveryOrchestrator::BriefRelayHealthState;
  LinkState next = link_state_;
  if (link_net_ == 0) {
    next = LinkState::Offline;
  } else if (link_net_ == 1) {
    if (link_health_ == Health::Ok) {
      next = LinkState::Online;
    } else if (link_health_ == Health::Failed || link_grace_over_) {
      next = LinkState::Connecting;
    } // else: no result yet, inside the grace period — keep what the line shows
  }
  if (next == link_state_) {
    return;
  }
  link_state_ = next;
  const auto callback = on_link_state_changed_;
  lock.unlock();
  if (callback) {
    callback(next);
  }
}

void ConversationsHub::SetOnStoredAttachmentsChanged(std::function<void()> callback) {
  std::lock_guard lock(storage_callbacks_mu_);
  on_stored_attachments_changed_ = std::move(callback);
}

void ConversationsHub::SetOnGridThumbnailReady(
    std::function<void(const std::string& thread_id, const std::vector<uint8_t>& content_hash)> callback) {
  std::lock_guard lock(storage_callbacks_mu_);
  on_grid_thumbnail_ready_ = std::move(callback);
}

void ConversationsHub::SetOnChatPreviewDone(
    std::function<void(const std::string& thread_id, const std::vector<uint8_t>& content_hash, bool made)> callback) {
  std::lock_guard lock(storage_callbacks_mu_);
  on_chat_preview_done_ = std::move(callback);
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
  return ProfileIconLocalPathForIdentity(data_dir_, *contacts_, identity);
}

void ConversationsHub::EnsureDirectoryHitIconCached(const mesh::DirectoryHit& hit) {
  ScheduleDirectoryHitIconFetch(hit);
}

void ConversationsHub::EnsureContactIconCached(const Contact& contact) {
  ScheduleContactIconFetch(contact);
}

bool ConversationsHub::RefreshContactProfileNow(const std::string& contact_id) {
  if (!contact_profile_refresh_ || !AppRuntime::IsRunning() || shutdown_requested_.load(std::memory_order_acquire)) {
    return false;
  }
  auto refreshed = contact_profile_refresh_->RefreshIfStale(contact_id, util::NowUnixMs());
  if (refreshed.updated) {
    EnsureContactIconCached(*refreshed.updated); // fetches the new photo and tells the screens when it is there
  }
  return refreshed.looked_up;
}

void ConversationsHub::RefreshContactProfileIfStale(const std::string& contact_id) {
  if (!contact_profile_refresh_ || !HasDirectory() || !AppRuntime::IsRunning() ||
      !contact_profile_refresh_->NeedsRefresh(contact_id, util::NowUnixMs())) {
    return;
  }
  AppRuntime::PostWorkerBackground([this, contact_id]() { RefreshContactProfileNow(contact_id); });
}

void ConversationsHub::RefreshStaleContactProfiles() {
  if (!contacts_ || !contact_profile_refresh_ || !HasDirectory() || !AppRuntime::IsRunning()) {
    return;
  }
  auto listed = contacts_->List();
  if (!listed || listed->empty()) {
    return;
  }
  auto ids = std::make_shared<std::vector<std::string>>();
  for (const Contact& contact : *listed) {
    ids->push_back(contact.id);
  }
  AppRuntime::PostWorkerBackground([this, ids]() { ContinueContactProfileSweep(ids, 0); });
}

void ConversationsHub::ContinueContactProfileSweep(std::shared_ptr<const std::vector<std::string>> contact_ids,
                                                   size_t index) {
  constexpr auto kSweepGap = std::chrono::milliseconds(500);
  bool looked_up = false;
  while (index < contact_ids->size() && !looked_up) {
    looked_up = RefreshContactProfileNow((*contact_ids)[index++]); // a fresh contact costs no lookup: next at once
  }
  // Quitting stops the sweep: no new lookup is queued behind the teardown.
  if (index >= contact_ids->size() || !AppRuntime::IsRunning() || shutdown_requested_.load(std::memory_order_acquire)) {
    return;
  }
  AppRuntime::ScheduleCoordinatorOneShot(kSweepGap, [this, contact_ids, index]() {
    AppRuntime::PostWorkerBackground([this, contact_ids, index]() { ContinueContactProfileSweep(contact_ids, index); });
  });
}

void ConversationsHub::ScheduleDirectoryHitIconFetch(const mesh::DirectoryHit& hit) {
  if (!hit.icon || hit.icon->url.empty()) {
    return;
  }
  const std::string key = ProfileIconCacheKeyForHit(hit);
  if (key.empty()) {
    return;
  }
  const mesh::ProfileIconRef icon = *hit.icon;
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
  constexpr auto kIconFetchRetryAfter = std::chrono::minutes(5);
  {
    std::lock_guard<std::mutex> lock(icon_fetch_mu_);
    if (icon_fetch_inflight_.count(key) > 0) {
      return;
    }
    const auto failed = icon_fetch_failed_at_.find(key);
    if (failed != icon_fetch_failed_at_.end() &&
        std::chrono::steady_clock::now() - failed->second < kIconFetchRetryAfter) {
      return;
    }
    icon_fetch_inflight_.insert(key);
  }
  const mesh::ProfileIconRef icon = *contact.remote.icon;
  AppRuntime::PostWorkerBackground([this, icon, key]() {
    bool fetched = false;
    bool failed = false;
    if (ProfileIconNeedsFetch(data_dir_, key, icon)) {
      fetched = static_cast<bool>(FetchProfileIcon(data_dir_, key, icon));
      failed = !fetched;
    }
    {
      std::lock_guard<std::mutex> lock(icon_fetch_mu_);
      icon_fetch_inflight_.erase(key);
      if (failed) {
        icon_fetch_failed_at_[key] = std::chrono::steady_clock::now();
      } else {
        icon_fetch_failed_at_.erase(key);
      }
    }
    if (fetched) {
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
  view.contact_locator = OwnContactLocator().valueOr("");
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
  PublishAccountLockerSoon(); // the locker keeps the nickname
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
  // privacy Y2: the directory is readable by anyone — a narrower audience registers the PeerId only
  // (peers reach us by it through relays; allowed peers learn addresses over ch0 / call signalling).
  if (mesh_ && mesh::PublishesAddresses(EffectiveDirectAudience())) {
    listen_addrs = mesh_->AdvertisedListenMultiaddrs();
    if (listen_addrs.empty() && mesh::IsUsableAdpListen(mesh_->AmpListenMultiaddr())) {
      listen_addrs.push_back(mesh_->AmpListenMultiaddr());
    }
  }
  auto applied = FinishAndPersistRegistration(Registration(), Identity(), identity->nickname, listen_addrs);
  if (!applied) {
    return applied.error();
  }
  PublishAccountLockerSoon(); // the locker keeps the relay binding: a phrase restore needs it
  return {};
}

Roe<void> ConversationsHub::RestoreFromRecoveryPhrase(const SecretText& phrase, const SecretText& pin,
                                                      const std::vector<std::string>& saved_copies) {
  if (!IsInitialized() || Secrets() == nullptr || Secrets()->Vault() == nullptr || !account_locker_) {
    return Error("Messaging is not ready");
  }
  auto seed = DecodeRecoveryPhrase(phrase);
  if (!seed) {
    return seed.error();
  }
  // The locker holds the relay binding (and the rest): without it there is no account to come back to here.
  auto fetched = account_locker_->Fetch(*seed, saved_copies);
  if (!fetched) {
    return Error("Account restore failed: " + fetched.error().message);
  }
  if (!*fetched) {
    return Error("No account was found for this recovery phrase");
  }
  AccountLockerState& state = (*fetched)->state;
  const int64_t now_ms = util::NowUnixMs();
  auto bundle = BuildRecoveryBundleJson(*seed, state.relay_user_id, state.nickname, now_ms);
  if (!bundle) {
    return Error("Account restore failed: " + bundle.error().message);
  }
  if (auto imported = LinkDeviceCoordinator::Import(Identity(), *Secrets()->Vault(), Secrets(), *bundle, pin, now_ms);
      !imported) {
    return Error("Account restore failed: " + imported.error().message);
  }
  for (const Contact& contact : state.contacts) {
    if (auto upserted = Contacts().Upsert(contact); !upserted) {
      return Error("Account restore failed: " + upserted.error().message);
    }
  }
  log().info << "Account restored from its recovery phrase: locker version " << (*fetched)->version << ", "
             << state.contacts.size() << " contact(s); " << state.extras.groups.size() << " group(s) and "
             << state.extras.card_floors.size() << " card floor(s) once messaging is up";
  QueueRestoredState(std::move(state.extras));
  return {};
}

void ConversationsHub::QueueRestoredState(AccountStateExtras extras) {
  {
    std::lock_guard lock(restored_mutex_);
    for (AccountStateGroup& group : extras.groups) {
      restored_.groups.push_back(std::move(group));
    }
    for (AccountCardFloor& floor : extras.card_floors) {
      restored_.card_floors.push_back(std::move(floor));
    }
    restored_.announce = true;
  }
  if (IsMessagingReady()) {
    ApplyRestoredCardFloors();
    AppRuntime::PostWorkerNormal([this]() {
      ResumeGroupRosterChecks();
      AnnounceRestoredDevice();
    });
  }
}

void ConversationsHub::PublishAccountLockerSoon() {
  if (!account_locker_ || !IsMessagingReady() || shutdown_requested_.load(std::memory_order_acquire)) {
    return;
  }
  if (locker_publish_queued_.exchange(true, std::memory_order_acq_rel)) {
    return; // the queued publish reads everything when it runs
  }
  // Batched: a burst of changes (a group's roster, its cards) becomes one snapshot.
  AppRuntime::ScheduleCoordinatorOneShot(std::chrono::milliseconds(kLockerPublishDelayMs), [this]() {
    AppRuntime::PostWorkerNormal([this]() {
      locker_publish_queued_.store(false, std::memory_order_release);
      if (!account_locker_ || !IsMessagingReady() || shutdown_requested_.load(std::memory_order_acquire)) {
        return;
      }
      auto seed = Identity().RecoverySeed();
      auto identity = Identity().Get();
      if (!seed || !*seed || !identity || identity->relay_user_id.empty()) {
        return; // an account from before M023, or not registered yet: nothing to publish
      }
      if (mesh_messaging_ && mesh_messaging_->OwnAccountCards().SignedOut()) {
        return; // M021 d4: a signed-out device no longer speaks for the account
      }
      if (auto published = account_locker_->Publish(); !published) {
        log().info << "Account locker not published: " << published.error().message;
      }
    });
  });
}

void ConversationsHub::ApplyRestoredCardFloors() {
  std::vector<AccountCardFloor> restored;
  {
    std::lock_guard lock(restored_mutex_);
    restored.swap(restored_.card_floors);
  }
  if (restored.empty() || !peer_cards_) {
    return;
  }
  std::vector<PeerCardFloor> floors;
  floors.reserve(restored.size());
  for (const AccountCardFloor& floor : restored) {
    floors.push_back({floor.account_id, floor.issued_at});
  }
  if (auto raised = peer_cards_->RaiseFloors(floors); !raised) {
    log().warning << "Card floors not restored: " << raised.error().message;
  }
}

void ConversationsHub::AnnounceRestoredDevice() {
  {
    std::lock_guard lock(restored_mutex_);
    if (!restored_.announce) {
      return;
    }
    restored_.announce = false;
  }
  AnnounceOwnDevicesToContacts();
}

void ConversationsHub::AnnounceOwnDevicesToContacts() {
  if (!IsMessagingReady() || !mesh_messaging_ || shutdown_requested_.load(std::memory_order_acquire)) {
    return;
  }
  auto contacts = Contacts().List();
  if (!contacts) {
    log().warning << "Own devices not announced: " << contacts.error().message;
    return;
  }
  std::vector<std::string> accounts;
  for (const Contact& contact : *contacts) {
    if (auto account = ContactAccountId(contact)) {
      accounts.push_back(*account);
    }
  }
  mesh_messaging_->NotifyContactsOfDevices(accounts);
}

void ConversationsHub::ResumeGroupRosterChecks() {
  if (!IsMessagingReady() || !group_membership_ || !group_roster_ ||
      shutdown_requested_.load(std::memory_order_acquire)) {
    return;
  }
  // G016: a dissolve or a refusal that did not get sent (the app quit, or messaging was down) goes now.
  group_membership_->ResumePendingGroupSends();
  // G018 item 4: changes held for a group we own (they arrived while we were away, or before a fold went out).
  group_membership_->FoldHeldChanges();
  std::vector<AccountStateGroup> restored;
  {
    std::lock_guard lock(restored_mutex_);
    restored.swap(restored_.groups);
  }
  for (const AccountStateGroup& group : restored) {
    if (group.left || group.dissolved) {
      continue; // a group the account is out of, or one that ended: nothing to bring back
    }
    if (auto recreated = group_membership_->RestoreGroup(group); !recreated) {
      log().warning << "Group not restored group=" << group.group_id << ": " << recreated.error().message;
    }
  }
  PublishAccountLockerSoon(); // at every start too: the locker catches up with what changed while it was off
  // Every group still checking its members (just restored, or the app quit mid-check) asks again.
  auto ids = group_roster_->ListGroupIds();
  if (!ids) {
    return;
  }
  for (const std::string& group_id : *ids) {
    auto metadata = group_roster_->LoadMetadata(group_id);
    if (metadata && *metadata && (*metadata)->sync_state == GroupSyncState::Checking) {
      if (auto asked = group_membership_->StartRosterCheck(group_id); !asked) {
        log().warning << "Roster check not started group=" << group_id << ": " << asked.error().message;
      }
    }
  }
}

Roe<void> ConversationsHub::UploadProfileIcon(const PreparedProfileIcon& prepared) {
  if (!IsInitialized()) {
    return Error("Messaging hub not initialized");
  }
  if (!IsMessagingReady()) {
    return AppError::Pin(Err::Pin::Required, "Unlock profile PIN to change profile icon");
  }
  if (!blob_) {
    return Error("Blob client not configured");
  }
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

/** The sender-side "too big" error: names the limit, in the words people read. */
Error AttachmentTooLargeError(const uint64_t max_bytes) {
  return AppError::Blob(Err::Blob::TooLarge, "Attachment over the size limit")
      .WithUser(Localized("chat.attachment.too_large", {{"limit", FormatAttachmentLimit(max_bytes)}}));
}

} // namespace

Roe<BlobQuotaRecoveryPlan> ConversationsHub::PlanRelayQuotaRecovery(const Error& quota_error) {
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
  if (!IsRelayFileQuotaError(quota_error)) {
    // `/blobs` holds the profile photo alone, under its own limit: nothing there for this device to free.
    return quota_error;
  }
  // An attachment found the files relay full: free the oldest sent files until this one fits.
  return PlanOldestRelayFileDeletion(*blob_, identity->relay_user_id, AttachmentQuotaNeededBytes(quota_error));
}

Roe<void> ConversationsHub::DeleteMessageForEveryone(const std::string& thread_id, const std::string& message_id) {
  if (!IsInitialized()) {
    return Error("Messaging hub not initialized");
  }
  auto thread = Store().GetThread(thread_id);
  if (!thread) {
    return thread.error();
  }
  if (!*thread || (*thread)->kind == ThreadKind::Ai) {
    return Error("Delete for everyone is only for chats");
  }
  auto found = Store().GetMessage(thread_id, message_id);
  if (!found) {
    return found.error();
  }
  if (!found->has_value()) {
    return Error("Message not found");
  }
  if ((*found)->content_type == ChatContentType::Deleted) {
    return {};
  }
  if (!CanDeleteForEveryone(**found)) {
    return Error("Only your own sent messages can be deleted for everyone");
  }
  const ThreadMessage original = **found;
  // A resend still queued for it must not go out after the retract.
  MeshMessaging().CancelPendingSend(message_id);
  if (auto sent = MeshMessaging().SendRetract(thread_id, message_id); !sent) {
    return sent.error();
  }
  if (auto deleted = Inbox().DeleteMessageForMe(thread_id, message_id); !deleted) {
    return deleted.error();
  }
  // Done for everyone once retracted and deleted here; the server copy is a separate, best-effort step.
  DeleteUploadedCopy(original);
  return {};
}

namespace {

/**
 * The edit's row after its send: what the send returned, or the row it left marked failed (`stored_unsent` set; the
 * message then shows the red mark).
 */
Roe<ThreadMessage> EditRowAfterSend(IThreadStore& store, const std::string& thread_id, const std::string& edit_id,
                                    Roe<ThreadMessage> sent, bool& stored_unsent) {
  if (sent) {
    return sent;
  }
  auto stored = store.GetMessage(thread_id, edit_id);
  if (!stored || !*stored) {
    return sent.error(); // refused before anything was stored: nothing changes here
  }
  stored_unsent = true;
  return **stored;
}

} // namespace

Roe<ThreadMessage> ConversationsHub::EditMessage(const std::string& thread_id, const std::string& message_id,
                                                 const std::string& new_text, const int64_t tapped_at_ms) {
  if (!IsInitialized()) {
    return Error("Messaging hub not initialized");
  }
  auto thread = Store().GetThread(thread_id);
  if (!thread) {
    return thread.error();
  }
  if (!*thread) {
    return Error("Thread not found");
  }
  auto found = Store().GetMessage(thread_id, message_id);
  if (!found) {
    return found.error();
  }
  if (!found->has_value()) {
    return Error("Message not found");
  }
  const ThreadMessage target = **found;
  if (!CanEditMessage(target, (*thread)->kind, tapped_at_ms)) {
    return Error("This message can no longer be edited");
  }
  if (new_text.empty() || new_text.size() > kMaxEditTextBytes) {
    return Error("Edited text is empty or too long");
  }
  if (new_text == target.text) {
    return target;
  }
  const std::string edit_id = util::GenerateUuid();
  bool stored_unsent = false;
  auto edit_row = EditRowAfterSend(
      Store(), thread_id, edit_id,
      MeshMessaging().SendEdit(thread_id, target.id, new_text, edit_id, EditSendTime(tapped_at_ms, target)),
      stored_unsent);
  if (!edit_row) {
    return edit_row.error();
  }
  if (stored_unsent) {
    log().warning << "edit stored but not sent (the message shows the red mark)";
  }
  auto edited = ApplyOwnEdit(Store(), target, *edit_row);
  if (!edited) {
    return edited.error();
  }
  Inbox().AfterMessageEdited(thread_id, target, *edited);
  return edited;
}

Roe<ThreadMessage> ConversationsHub::ResendEdit(const std::string& thread_id, const std::string& message_id) {
  if (!IsInitialized()) {
    return Error("Messaging hub not initialized");
  }
  auto found = Store().GetMessage(thread_id, message_id);
  if (!found || !found->has_value()) {
    return Error("Message not found");
  }
  const ThreadMessage target = **found;
  const std::optional<EditMark> mark = EditMarkOf(target);
  if (target.sender_contact_id != kLocalSelfContactId || target.content_type != ChatContentType::Text || !mark) {
    return Error("Not an edited message of ours");
  }
  auto unsent = Store().GetMessage(thread_id, mark->edited_by);
  if (!unsent || !unsent->has_value()) {
    return Error("Its edit is gone");
  }
  const ThreadMessage old_edit = **unsent;
  const std::optional<std::string> text = EditTextOf(old_edit);
  if (!text || text->empty() || old_edit.delivery == MessageDelivery::Relayed) {
    return Error("Nothing to send again");
  }
  // Same words, same send time (the time its receivers judge); a new id, as a re-sent message has.
  const std::string edit_id = util::GenerateUuid();
  bool stored_unsent = false;
  auto edit_row = EditRowAfterSend(Store(), thread_id, edit_id,
                                   MeshMessaging().SendEdit(thread_id, target.id, *text, edit_id, old_edit.timestamp),
                                   stored_unsent);
  if (!edit_row) {
    return edit_row.error();
  }
  if (stored_unsent) {
    log().warning << "resent edit stored but not sent";
  }
  if (auto edited = ApplyOwnEdit(Store(), target, *edit_row); !edited) {
    return edited.error();
  }
  MeshMessaging().CancelPendingSend(old_edit.id);
  if (auto removed = Store().RemoveUnsentMessage(thread_id, old_edit.id); !removed || !*removed) {
    log().warning << "resent edit: the unsent edit row was not removed";
  }
  return edit_row;
}

void ConversationsHub::DeleteUploadedCopy(const ThreadMessage& message) {
  if (message.content_type != ChatContentType::Attachment || !blob_) {
    return;
  }
  auto fields = ChatPayloadCodec::DecodeAttachmentJson(message.payload_json);
  if (!fields || fields->url.empty()) {
    return; // sent peer-direct: nothing on the relay
  }
  auto identity = Identity().Get();
  if (!identity || identity->relay_user_id.empty()) {
    return;
  }
  if (auto removed = DeleteUploadedAttachmentCopy(*blob_, identity->relay_user_id, fields->url); !removed) {
    // The copy expires on its own (7 days); the detail names the blob id, never the link.
    log().warning << "uploaded copy not deleted: " << AppError::Log(removed.error());
  }
}

Roe<void> ConversationsHub::DeleteRelayFileById(const std::string& blob_id) {
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
  return DeleteRelayFile(*blob_, identity->relay_user_id, blob_id);
}

AttachmentLimits ConversationsHub::CurrentAttachmentLimits() const {
  return DefaultAttachmentLimits();
}

void ConversationsHub::SetOnBlobSpaceWarning(std::function<void(int percent)> callback) {
  std::lock_guard lock(blob_space_mutex_);
  on_blob_space_warning_ = std::move(callback);
}

void ConversationsHub::CheckBlobSpace(const bool after_quota_error) {
  if (!blob_space_monitor_.ShouldCheck(util::NowUnixMs(), after_quota_error) || !blob_) {
    return;
  }
  auto identity = Identity().Get();
  if (!identity || !identity->registered || identity->relay_user_id.empty()) {
    return;
  }
  // Attachments live on the files relay (#465): its byte quota is the space that fills.
  auto listed = blob_->FileList(identity->relay_user_id, "");
  if (!listed) {
    return;
  }
  const AttachmentLimits limits = CurrentAttachmentLimits();
  const int percent = BlobSpaceUsedPercent(
      BlobSpaceUse{.bytes_used = listed->usage.bytes_used, .quota_bytes = listed->usage.quota_bytes});
  if (blob_space_monitor_.NextWarning(percent, limits) == 0) {
    return;
  }
  std::function<void(int)> callback;
  {
    std::lock_guard lock(blob_space_mutex_);
    callback = on_blob_space_warning_;
  }
  if (callback) {
    AppRuntime::PostUI([callback = std::move(callback), percent]() { callback(percent); });
  }
}

Roe<Thread> ConversationsHub::AttachmentTargetThread(const std::string& thread_id) {
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
  if ((*thread)->kind == ThreadKind::Ai) {
    return Error("Attachments are not supported in assistant threads");
  }
  if ((*thread)->kind != ThreadKind::Direct && (*thread)->kind != ThreadKind::Group) {
    return Error("Attachments are not supported in this thread");
  }
  return **thread;
}

void ConversationsHub::SendAttachmentAsync(const std::string& thread_id, OutgoingAttachment attachment,
                                           std::function<void(Roe<ThreadMessage>)> on_done,
                                           std::function<bool(uint64_t done, uint64_t total)> progress,
                                           std::shared_ptr<const std::atomic<bool>> cancelled) {
  auto finish = [on_done = std::move(on_done)](Roe<ThreadMessage> value) {
    if (on_done) {
      on_done(std::move(value));
    }
  };
  auto target = AttachmentTargetThread(thread_id);
  if (!target) {
    finish(target.error());
    return;
  }
  Thread active = std::move(*target);

  const AttachmentLimits limits = CurrentAttachmentLimits();
  const uint64_t max_bytes = MaxAttachmentSendBytes(limits, attachment.mime, attachment.image_is_shrunk);
  if (attachment.bytes.size() > max_bytes) {
    finish(AttachmentTooLargeError(max_bytes));
    return;
  }
  if (!attachment_send_limiter_.TryAcquire(util::NowUnixMs(), limits.send_max_per_minute)) {
    finish(AppError::Blob(Err::Blob::RateLimited, "Attachment send pace exceeded"));
    return;
  }

  auto store = std::make_shared<OutgoingAttachmentStore>(data_dir_, profile_id_);
  auto job = std::make_shared<AttachmentSendJob>();
  job->thread_id = thread_id;
  job->kind = active.kind;
  job->store = store;
  job->dek = Attachments().CopyDek();
  job->started_ms = util::NowUnixMs();
  job->bytes = attachment.bytes.size();
  // The bytes live until the ciphertext is staged on disk (below): the upload streams from that file, and the
  // sent copy in the content store is then rebuilt from it, so a large file is not held twice while it uploads.
  job->plaintext = std::make_shared<const ByteVector>(std::move(attachment.bytes));

  ChatAttachmentUploadOptions upload_opts;
  upload_opts.peer_client = mesh_messaging_ ? mesh_messaging_->PeerBlobClient() : nullptr;
  upload_opts.contacts = contacts_.get();
  upload_opts.thread = active;
  upload_opts.thread_id = thread_id;
  upload_opts.max_plaintext_bytes = max_bytes;
  upload_opts.progress = LaneAwareProgress(std::move(progress));
  upload_opts.cancelled = std::move(cancelled);
  if (!job->dek.empty() && !profile_id_.empty()) {
    upload_opts.outgoing = store.get(); // staged: a failed send can be resumed without the original file
    upload_opts.dek = job->dek;
    upload_opts.on_staged = [job](const OutgoingAttachmentRecord& record) {
      job->record_id = record.id;
      job->plaintext.reset();
    };
  }

  auto run = [this, job, finish, name = attachment.filename, mime = attachment.mime,
              upload_opts = std::move(upload_opts)]() mutable {
    UploadChatAttachmentAsync(*blob_, Identity(), *job->plaintext, name, mime, std::move(upload_opts),
                              [this, job, finish](Roe<ChatAttachmentFields> fields) mutable {
                                auto continue_send = [this, job, finish = std::move(finish),
                                                      fields = std::move(fields)]() mutable {
                                  FinishAttachmentSend(job, std::move(fields), finish);
                                };
                                if (AppRuntime::IsRunning()) {
                                  AppRuntime::PostWorkerNormal(std::move(continue_send));
                                } else {
                                  continue_send();
                                }
                              });
  };
  // A large file is encrypted, staged and uploaded on the upload lane, not on a shared worker (minutes).
  if (job->bytes > kMaxDirectAttachmentBytes && AppRuntime::IsRunning()) {
    transfer_lane_.Post(LargeTransferLane::Direction::Upload, std::move(run));
  } else {
    run();
  }
}

void ConversationsHub::ResumeAttachmentAsync(const std::string& thread_id, const std::string& record_id,
                                             std::function<void(Roe<ThreadMessage>)> on_done,
                                             std::function<bool(uint64_t done, uint64_t total)> progress,
                                             std::shared_ptr<const std::atomic<bool>> cancelled) {
  // Never on the caller's thread (the UI's): the record is read, a small file may be pushed straight to the
  // contact, and the upload itself is blocking work. A large file goes to the upload lane.
  auto body = [this, thread_id, record_id, on_done = std::move(on_done), progress = std::move(progress),
               cancelled = std::move(cancelled)]() mutable {
    ResumeAttachmentNow(thread_id, record_id, std::move(on_done), std::move(progress), std::move(cancelled));
  };
  if (AppRuntime::IsRunning()) {
    AppRuntime::PostWorkerNormal(std::move(body));
  } else {
    body();
  }
}

void ConversationsHub::ResumeAttachmentNow(const std::string& thread_id, const std::string& record_id,
                                           std::function<void(Roe<ThreadMessage>)> on_done,
                                           std::function<bool(uint64_t done, uint64_t total)> progress,
                                           std::shared_ptr<const std::atomic<bool>> cancelled) {
  auto finish = [on_done = std::move(on_done)](Roe<ThreadMessage> value) {
    if (on_done) {
      on_done(std::move(value));
    }
  };
  auto target = AttachmentTargetThread(thread_id);
  if (!target) {
    finish(target.error());
    return;
  }
  const ByteVector dek = Attachments().CopyDek();
  if (dek.empty()) {
    // Locked: nothing is wrong with the record, it just cannot be read now. Kept; unlock and retry.
    finish(AppError::Pin(Err::Pin::Required, "Unlock the profile to send the attachment again"));
    return;
  }
  auto store = std::make_shared<OutgoingAttachmentStore>(data_dir_, profile_id_);
  const auto pick_again = [&](const std::string& why) {
    return AppError::Blob(Err::Blob::Expired, why).WithUser(Localized("chat.attachment.pick_again"));
  };
  auto record = store->Load(record_id, dek);
  if (!record) {
    // Only a record that is confirmed gone or damaged is deleted; any other failure leaves it for a later try.
    if (record.error().code == OutgoingAttachmentStore::kLoadGone) {
      finish(pick_again("Outgoing attachment record is gone"));
    } else if (record.error().code == OutgoingAttachmentStore::kLoadDamaged) {
      store->Remove(record_id);
      finish(pick_again("Outgoing attachment record is damaged"));
    } else {
      finish(record.error());
    }
    return;
  }
  if (record->thread_id != thread_id) {
    // A chip left over from another thread: nothing is changed.
    finish(Error(OutgoingAttachmentStore::kWrongThread, "Outgoing attachment belongs to another thread"));
    return;
  }
  if (!record->delivered && !store->HasCipher(record_id)) {
    store->Remove(record_id); // the encrypted copy is confirmed lost: the person picks the file again
    finish(pick_again("Outgoing attachment ciphertext is gone"));
    return;
  }
  auto job = std::make_shared<AttachmentSendJob>();
  job->thread_id = thread_id;
  job->kind = target->kind;
  job->store = store;
  job->record_id = record_id;
  job->dek = dek;
  job->started_ms = util::NowUnixMs();
  job->bytes = record->fields.byte_length;

  ChatAttachmentUploadOptions upload_opts;
  upload_opts.peer_client = mesh_messaging_ ? mesh_messaging_->PeerBlobClient() : nullptr;
  upload_opts.contacts = contacts_.get();
  upload_opts.thread = *target;
  upload_opts.thread_id = thread_id;
  upload_opts.progress = LaneAwareProgress(std::move(progress));
  upload_opts.cancelled = std::move(cancelled);
  upload_opts.outgoing = store.get();
  upload_opts.dek = dek;
  auto run = [this, job, finish, record = std::move(*record), upload_opts = std::move(upload_opts)]() mutable {
    ResumeChatAttachmentAsync(*blob_, Identity(), record, std::move(upload_opts),
                              [this, job, finish](Roe<ChatAttachmentFields> fields) mutable {
                                auto continue_send = [this, job, finish = std::move(finish),
                                                      fields = std::move(fields)]() mutable {
                                  FinishAttachmentSend(job, std::move(fields), finish);
                                };
                                if (AppRuntime::IsRunning()) {
                                  AppRuntime::PostWorkerNormal(std::move(continue_send));
                                } else {
                                  continue_send();
                                }
                              });
  };
  if (job->bytes > kMaxDirectAttachmentBytes && AppRuntime::IsRunning()) {
    transfer_lane_.Post(LargeTransferLane::Direction::Upload, std::move(run));
  } else {
    run();
  }
}

std::function<bool(uint64_t done, uint64_t total)> ConversationsHub::LaneAwareProgress(
    std::function<bool(uint64_t done, uint64_t total)> progress) {
  // Shutdown aborts a transfer in flight at its next progress tick.
  return [this, progress = std::move(progress)](const uint64_t done, const uint64_t total) {
    if (transfer_lane_.Stopping()) {
      return false;
    }
    return progress ? progress(done, total) : true;
  };
}

std::vector<OutgoingAttachmentRecord> ConversationsHub::PendingOutgoingAttachments(const std::string& thread_id) {
  const ByteVector dek = Attachments().CopyDek();
  if (dek.empty() || profile_id_.empty() || data_dir_.empty()) {
    return {};
  }
  return OutgoingAttachmentStore(data_dir_, profile_id_).ListForThread(thread_id, dek);
}

void ConversationsHub::DiscardOutgoingAttachment(const std::string& record_id) {
  OutgoingAttachmentStore(data_dir_, profile_id_).Remove(record_id);
}

void ConversationsHub::CleanStaleAttachmentFiles() {
  if (data_dir_.empty()) {
    return;
  }
  OutgoingAttachmentStore(data_dir_, profile_id_).CleanupStale(OutgoingAttachmentStore::kMaxAgeMs);
  CleanIncomingCiphertext(data_dir_); // a download that never finished
  std::error_code picks_ec;
  std::filesystem::remove_all(std::filesystem::path(data_dir_) / "picked", picks_ec); // copies of content:// picks
  (void)WipeAllAttachmentViewCaches(data_dir_); // plaintext views live for a session only: none from the last one
}

void ConversationsHub::FinishAttachmentSend(const std::shared_ptr<AttachmentSendJob>& job,
                                            Roe<ChatAttachmentFields> fields,
                                            const std::function<void(Roe<ThreadMessage>)>& finish) {
  const auto elapsed = [&]() { return util::NowUnixMs() - job->started_ms; };
  if (!fields) {
    EmitAttachmentTransferMetric("send", job->bytes, elapsed(), false);
    if (IsBlobQuotaError(fields.error())) {
      CheckBlobSpace(/*after_quota_error=*/true);
    }
    // A full space is answered by the delete-and-retry dialog, which resumes from this staged ciphertext (so it
    // stays); a size the server refuses cannot succeed by retrying. Every other failure keeps it for "Retry".
    const bool keep = !(fields.error().category == static_cast<int32_t>(ErrorCategory::Blob) &&
                        fields.error().code == static_cast<int32_t>(Err::Blob::TooLarge));
    if (!keep && !job->record_id.empty()) {
      job->store->Remove(job->record_id);
    }
    finish(fields.error());
    return;
  }

  SendRelayOptions opts;
  opts.content_type = ChatContentType::Attachment;
  opts.payload_json = ChatPayloadCodec::AttachmentFieldsToJson(*fields);
  const std::string display = fields->filename.empty() ? "Attachment" : fields->filename;

  const ByteVector attachment_dek = Attachments().CopyDek();
  const uint64_t max_receive_bytes = CurrentAttachmentLimits().max_receive_bytes;

  const std::shared_ptr<CasStore> content = content_;
  const auto keep_sent_copy = [&](const ThreadMessage& sent) {
    if (!attachment_dek.empty() && content) {
      if (job->plaintext) {
        (void)CopyAttachmentPlaintext(data_dir_, *content, job->thread_id, *fields, *job->plaintext, attachment_dek,
                                      max_receive_bytes);
        Attachments().MaybeBuildImageThumbnail(job->thread_id, *fields, *job->plaintext); // a large picture we sent
      } else if (!job->record_id.empty()) {
        // A resumed send has no plaintext in memory: it is rebuilt from the staged ciphertext and its key. The
        // ciphertext is moved into Decrypt, which frees it before returning: only the plaintext is held for the copy.
        if (auto cipher = job->store->ReadCipher(job->record_id)) {
          if (auto plain = AttachmentContentCipher::Decrypt(fields->content_key, fields->blob_nonce, std::move(*cipher),
                                                            fields->content_hash)) {
            (void)CopyAttachmentPlaintext(data_dir_, *content, job->thread_id, *fields, *plain, attachment_dek,
                                          max_receive_bytes);
            Attachments().MaybeBuildImageThumbnail(job->thread_id, *fields, *plain);
          }
        }
      }
    }
    Attachments().MaybeBuildPoster(job->thread_id, *fields);
    if (attachment_downloads_) {
      attachment_downloads_->EnqueueFromMessage(job->thread_id, sent);
    }
    if (!fields->url.empty()) {
      CheckBlobSpace(/*after_quota_error=*/false); // went through the server: it holds our copy
    }
    if (!job->record_id.empty()) {
      job->store->Remove(job->record_id); // sent: the staged ciphertext and key are no longer needed
    }
  };

  // Me → Storage may be deleting the same bytes: they are held (without waiting) until the message and the sent copy
  // are written, so a delete running or scanning meanwhile keeps them.
  Attachments().AcquireBytesForSend(job->thread_id, fields->content_hash);
  auto sent = job->kind == ThreadKind::Group ? MeshMessaging().SendGroupMessage(job->thread_id, display, opts)
                                             : MeshMessaging().SendUserMessage(job->thread_id, display, opts);
  if (sent) {
    keep_sent_copy(*sent);
  }
  Attachments().ReleaseBytesForSend(fields->content_hash);
  EmitAttachmentTransferMetric("send", job->bytes, elapsed(), static_cast<bool>(sent));
  finish(std::move(sent));
}

AttachmentFetchWorkflow& ConversationsHub::Attachments() {
  if (!attachment_downloads_) {
    attachment_downloads_ = std::make_unique<AttachmentFetchWorkflow>();
    attachment_downloads_->SetProfileDataDir(data_dir_);
    attachment_downloads_->SetContent(content_);
    attachment_downloads_->SetLimits(CurrentAttachmentLimits());
  }
  return *attachment_downloads_;
}

void ConversationsHub::FeedAttachmentNetwork() {
  if (!attachment_downloads_) {
    return;
  }
  // A desktop is always on Wi-Fi; a phone waits for its first network report (until then large files stay queued).
  const AutoDownloadNetwork network =
      host_.mobile ? attachment_network_.load() : AutoDownloadNetworkOf(NetworkState{}, /*mobile_host=*/false);
  if (network == AutoDownloadNetwork::Unknown) {
    return;
  }
  attachment_downloads_->SetAutoAllowedOnNetwork(
      AutoDownloadAllowedOn(network, policy_.attachment_auto_on_wifi, policy_.attachment_auto_on_cellular));
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
  attachment_downloads_->SetContent(content_);
  attachment_downloads_->SetLimits(CurrentAttachmentLimits());
  attachment_downloads_->SetTransferLane(&transfer_lane_);
  attachment_downloads_->SetFetchDependencies(&Store(), contacts_.get(), identity_.get(),
                                              mesh_messaging_ ? mesh_messaging_->PeerBlobClient() : nullptr);
  attachment_downloads_->SetSuppressionStore(attachment_suppressions_.get());
  attachment_downloads_->SetDownloadPolicy(EffectiveAttachmentDownloadPolicy(
      policy_.attachment_download_policy, policy_.attachment_auto_on_wifi, policy_.attachment_auto_on_cellular));
  FeedAttachmentNetwork();
  if (host_.mobile && attachment_network_.load() == AutoDownloadNetwork::Unknown &&
      !attachment_network_timer_armed_.exchange(true)) {
    // No network report yet (none may ever come without a mesh): after a while count it as the mobile network,
    // the careful side, so large files show "Download" instead of waiting queued for good. One timer at a time
    // (a rewire keeps the pending one); bound to the hub's run, so Shutdown (or the hub going) makes it a no-op.
    AppRuntime::ScheduleCoordinatorOneShot(std::chrono::seconds(15), attachment_network_lifetime_.Bind([this]() {
      attachment_network_timer_armed_.store(false);
      AutoDownloadNetwork unknown = AutoDownloadNetwork::Unknown;
      if (attachment_network_.compare_exchange_strong(unknown, AutoDownloadNetwork::Mobile)) {
        AppRuntime::PostUI(attachment_network_lifetime_.Bind([this]() { FeedAttachmentNetwork(); }));
      }
    }));
  }
  // Bound to the hub's run: a report still queued when it shuts down (or goes) never reaches it.
  attachment_downloads_->SetOnChanged(storage_callbacks_lifetime_.Bind([this]() {
    if (mesh_messaging_) {
      mesh_messaging_->NotifyMessagesChanged();
    }
    std::function<void()> callback;
    {
      std::lock_guard lock(storage_callbacks_mu_);
      callback = on_stored_attachments_changed_;
    }
    if (callback) {
      AppRuntime::PostUI(storage_callbacks_lifetime_.Bind(std::move(callback)));
    }
  }));
  // A grid cell's thumbnail: only that cell is refreshed (no chat re-render, no recount of the storage lists).
  attachment_downloads_->SetOnGridThumbnailReady(
      storage_callbacks_lifetime_.Bind([this](const std::string& thread_id, const std::vector<uint8_t>& content_hash) {
        std::function<void(const std::string&, const std::vector<uint8_t>&)> callback;
        {
          std::lock_guard lock(storage_callbacks_mu_);
          callback = on_grid_thumbnail_ready_;
        }
        if (callback) {
          callback(thread_id, content_hash);
        }
      }));
  // A chat picture's preview: only the rows showing it are redrawn (no whole-chat re-render).
  attachment_downloads_->SetOnChatPreviewDone(storage_callbacks_lifetime_.Bind(
      [this](const std::string& thread_id, const std::vector<uint8_t>& content_hash, const bool made) {
        std::function<void(const std::string&, const std::vector<uint8_t>&, bool)> callback;
        {
          std::lock_guard lock(storage_callbacks_mu_);
          callback = on_chat_preview_done_;
        }
        if (callback) {
          callback(thread_id, content_hash, made);
        }
      }));
  if (inbox_) {
    inbox_->SetProfileDataDir(data_dir_);
    inbox_->SetAttachmentDownloads(attachment_downloads_.get());
  }
  if (mesh_messaging_) {
    mesh_messaging_->SetAttachmentDownloads(attachment_downloads_.get());
    if (auto* peer_blob = mesh_messaging_->PeerBlobService()) {
      // The receiver's own pace and disk limits apply to what a contact pushes straight to this device too.
      peer_blob->SetPushGate([workflow = attachment_downloads_.get()](const std::string& sender, const uint64_t bytes) {
        return workflow->TryAcceptDirectPush(sender, bytes);
      });
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

std::vector<StoredAttachmentItem> ConversationsHub::ListStoredAttachments() {
  if (!IsInitialized()) {
    return {};
  }
  return Attachments().ListStoredAttachments(Store());
}

uint64_t ConversationsHub::ReleasableAttachmentBytes(const std::vector<StoredAttachmentRef>& items) {
  if (!IsInitialized()) {
    return 0;
  }
  return Attachments().ReleasableBytes(Store(), items);
}

void ConversationsHub::DeleteLocalAttachments(std::vector<StoredAttachmentRef> items,
                                              std::function<void(StoredAttachmentDeleteResult)> done) {
  if (!IsInitialized()) {
    if (done) {
      done(StoredAttachmentDeleteResult{});
    }
    return;
  }
  Attachments().DeleteLocalAttachments(Store(), std::move(items), std::move(done));
}

uint64_t ConversationsHub::MessageStoreBytes() const {
  return message_store_bytes_ ? message_store_bytes_() : 0;
}

void ConversationsHub::RequestGridThumbnail(const std::string& thread_id, const std::vector<uint8_t>& content_hash,
                                            const std::string& mime, const uint64_t page_generation) {
  if (!IsInitialized()) {
    return;
  }
  Attachments().RequestGridThumbnail(thread_id, content_hash, mime, page_generation);
}

void ConversationsHub::CancelGridThumbnails(const uint64_t page_generation) {
  if (attachment_downloads_) {
    attachment_downloads_->CancelGridThumbnails(page_generation);
  }
}

void ConversationsHub::CancelChatPreviews() {
  if (attachment_downloads_) {
    attachment_downloads_->CancelChatPreviews();
  }
}

std::string ConversationsHub::GridThumbnailPath(const std::string& thread_id,
                                                const std::vector<uint8_t>& content_hash) {
  if (!IsInitialized()) {
    return {};
  }
  return Attachments().GridThumbnailPath(thread_id, content_hash);
}

Roe<void> ConversationsHub::SendChargeRequired(const std::string& peer_identity,
                                               const std::optional<int64_t> floor_minor) {
  if (!IsInitialized() || !IsMessagingReady() || !mesh_messaging_) {
    return Error("Messaging not ready");
  }
  return mesh_messaging_->SendChargeRequired(peer_identity, floor_minor);
}

AccountSigningKeyLookup ConversationsHub::AccountSigningKeys() const {
  return [keys = account_keys_.get()](const std::string& account_id) -> Roe<ByteVector> {
    if (!keys) {
      return Error("account signing keys unavailable");
    }
    auto record = keys->Signing().Resolve(ContactIdKindToString(mesh::ContactIdKind::Account), account_id);
    if (!record) {
      return record.error();
    }
    return Base64Decode(record->signing_public_key_b64);
  };
}

void ConversationsHub::BindCardAskFamily() {
  // M021 d6: another account was told our list is newer than the one it keeps — it asks; we send the card.
  system_controls_.Bind(card_ask::MakeFamily([this](std::string to_device, std::string asker) {
    AppRuntime::PostWorkerNormal([this, to_device = std::move(to_device), asker = std::move(asker)]() {
      if (mesh_messaging_) {
        mesh_messaging_->AnswerCardAsk(to_device, asker);
      }
    });
  }));
}

void ConversationsHub::BindGroupLinkJoinFamily() {
  // G021: a stranger asks to join by one of our links, or answers our ask — judged off the ingest.
  const auto post = [this](auto work) {
    AppRuntime::PostWorkerNormal([this, work = std::move(work)]() {
      if (group_links_) {
        work(*group_links_);
      }
    });
  };
  system_controls_.Bind(MakeGroupLinkJoinFamily(
      [identity = identity_.get()]() -> std::optional<std::string> {
        auto account = identity->GetAccountId();
        if (!account || account->empty()) {
          return std::nullopt;
        }
        return *account;
      },
      GroupLinkJoinHandlers{
          .on_request =
              [post](GroupJoinRequest request, std::string joiner) {
                post([request = std::move(request), joiner = std::move(joiner)](GroupInviteLinkWorkflow& links) {
                  links.OnRequest(request, joiner);
                });
              },
          .on_preview =
              [post](GroupJoinPreview preview, std::string answerer) {
                post([preview = std::move(preview), answerer = std::move(answerer)](GroupInviteLinkWorkflow& links) {
                  links.OnPreview(preview, answerer);
                });
              },
          .on_refused =
              [post](GroupJoinRefused refused, std::string answerer) {
                post([refused = std::move(refused), answerer = std::move(answerer)](GroupInviteLinkWorkflow& links) {
                  links.OnRefused(refused, answerer);
                });
              },
          .on_confirm =
              [post](GroupJoinConfirm confirm, std::string joiner) {
                post([confirm = std::move(confirm), joiner = std::move(joiner)](GroupInviteLinkWorkflow& links) {
                  links.OnConfirm(confirm, joiner);
                });
              },
      }));
}

void ConversationsHub::BindCopyAskFamily() {
  // M021 §9: another account's device asks for a copy of what we sent it never got — answered off the ingest.
  system_controls_.Bind(copy_ask::MakeFamily([this](CopyAsk ask, std::string asker) {
    AppRuntime::PostWorkerNormal([this, ask = std::move(ask), asker = std::move(asker)]() {
      if (mesh_messaging_) {
        mesh_messaging_->AnswerCopyAsk(ask, asker);
      }
    });
  }));
}

void ConversationsHub::BindGroupRosterSyncFamily() {
  system_controls_.Bind(MakeGroupRosterSyncFamily(
      *group_roster_,
      [identity = identity_.get()]() -> std::optional<std::string> {
        auto account = identity->GetAccountId();
        if (!account || account->empty()) {
          return std::nullopt;
        }
        return *account;
      },
      [this](group_roster_sync_codec::RosterRequest request, std::string asker) {
        // Off the ingest: the owner signs its answer.
        AppRuntime::PostWorkerNormal([this, request = std::move(request), asker = std::move(asker)]() {
          if (group_membership_) {
            if (auto answered = group_membership_->AnswerRosterRequest(request, asker); !answered) {
              log().warning << "Roster answer to " << asker << " failed: " << answered.error().message;
            }
          }
        });
      },
      [this](GroupDissolvedEnd end) {
        // G016: a group ended by an answer to our check, or by our own other device: its line and notice, off the ingest.
        AppRuntime::PostWorkerNormal([this, end = std::move(end)]() {
          if (group_membership_) {
            group_membership_->OnGroupDissolvedElsewhere(end);
          }
        });
      },
      GroupRosterSyncHooks{
          .cards = [this](const std::string& account) -> std::optional<AccountCard> {
            return mesh_messaging_ ? mesh_messaging_->GroupChangeCards()(account) : std::nullopt;
          },
          // G018 item 5: the changes a reply brought, their lines (and the owner's fold), off the ingest.
          .on_applied =
              [this](std::string group_id, std::vector<GroupChange> applied) {
                AppRuntime::PostWorkerNormal([this, group_id = std::move(group_id), applied = std::move(applied)]() {
                  if (group_membership_) {
                    group_membership_->OnChangesApplied(group_id, applied);
                  }
                });
              },
          .ask_more =
              [this](std::string group_id, std::string replier, std::string after) {
                AppRuntime::PostWorkerNormal(
                    [this, group_id = std::move(group_id), replier = std::move(replier), after = std::move(after)]() {
                      if (group_membership_) {
                        (void)group_membership_->StartCatchUp(group_id, replier, after);
                      }
                    });
              },
      }));
}

void ConversationsHub::BindPricingFamilies() {
  system_controls_.Bind(MakeInitiationBillingFamily(*initiation_billing_, [identity = identity_.get()]() -> int64_t {
    auto local = identity->Get();
    return local ? local->initiation_floor : 0;
  }));
  system_controls_.Bind(MakePaymentPromiseFamily(
      *payment_promises_,
      [identity = identity_.get()]() -> std::optional<std::string> {
        auto account = identity->GetAccountId();
        if (!account || account->empty()) {
          return std::nullopt;
        }
        return *account;
      },
      AccountSigningKeys()));
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
  return PaymentPromiseLifecycle::CommitInbound(*payment_promises_, Identity(), AccountSigningKeys(), promise_id);
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

Roe<void> ConversationsHub::RotateBriefLlmKey(const std::string& llm_base_url) {
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
        .WithUser(Localized("chat.hint.brief_register"));
  }

  if (llm_base_url.empty()) {
    return AppError::Config(Err::Config::Invalid, "Brief LLM base URL not configured");
  }
  const std::string url = llm_base_url + "/keys/rotate";

  auto response = HttpClient::Post(
      url, "{}",
      {{"Authorization", "Bearer " + identity->brief_llm_api_key.Reveal()}, {"Content-Type", "application/json"}});
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
    return AppError::Auth(Err::Auth::Forbidden, "Brief key rotate HTTP " + std::to_string(response->status_code))
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
        .WithUser(Localized("chat.hint.brief_key_save_failed"));
  }
  const std::string new_key = *new_key_opt;
  if (new_key.empty()) {
    return AppError::Auth(Err::Auth::Generic, "Brief API key rotate returned empty key")
        .WithUser(Localized("chat.hint.brief_key_save_failed"));
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
  if (!IsHelpNetworkEnabled()) {
    return;
  }
  const mesh::ReachabilitySnapshot snap = Reachability();
  if (snap.status != mesh::ReachabilityStatus::OutboundOnly && snap.status != mesh::ReachabilityStatus::Blocked) {
    reachability_outbound_since_ms_ = 0;
    return;
  }
  const auto now = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
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

void ConversationsHub::RefreshMeshCapabilities(const MeshConfig& mesh) {
  if (!messaging_ready_ || !mesh_) {
    return;
  }
  config_.mesh = mesh;
  PublishMeshConfig();
  // Rewire sequence (L015): dependents let go of the mesh objects → reset → rewire → rebind.
  for (HubParticipant* participant : ParticipantsDown()) {
    participant->OnMeshMediaDetach();
  }
  if (mesh_owner_) {
    mesh_owner_->DetachMeshObjects();
  }
  if (mesh_owner_) {
    mesh_owner_->ApplyServing(config_.mesh); // what the role serves inbound (circuit, media relay)
  }
  ConfigureMeshQueries();
  ApplyMeshAdmissionPolicies();
  if (mesh_owner_) {
    mesh_owner_->WireMeshObjects(); // the hop policy pushes now: the role may have changed what is a candidate
  }
  for (HubParticipant* participant : Participants()) {
    participant->OnMeshMediaRebound(*this);
  }
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
      next.node_enabled != config_.mesh.node_enabled || next.circuit_relay != config_.mesh.capabilities.circuit_relay ||
      next.media_relay != config_.mesh.capabilities.media_relay || next.dht != config_.mesh.capabilities.dht ||
      next.prefer_contacts_for_routing != config_.mesh.prefer_contacts_for_routing ||
      next.trusted_relays_only != config_.mesh.trusted_relays_only; // relay deps are built at Wire

  config_.relay = next.relay;
  config_.directory = next.directory;
  config_.registration = next.registration;
  config_.mesh.node_enabled = next.node_enabled;
  config_.mesh.capabilities.circuit_relay = next.circuit_relay;
  config_.mesh.capabilities.media_relay = next.media_relay;
  config_.mesh.capabilities.dht = next.dht;
  config_.mesh.prefer_contacts_for_routing = next.prefer_contacts_for_routing;
  config_.mesh.direct_connections = next.direct_connections;
  config_.mesh.trusted_relays_only = next.trusted_relays_only;
  config_.mesh.reachable_without_relay = next.reachable_without_relay;
  PublishMeshConfig();
  SyncStayReachable(); // republishes who may learn our address too

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
    RefreshMeshCapabilities(next.mesh);
  }
}

void ConversationsHub::Apply(const PolicyPrefs& prefs) {
  policy_ = prefs; // a stack built later takes it too
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
    attachment_downloads_->SetDownloadPolicy(EffectiveAttachmentDownloadPolicy(
        prefs.attachment_download_policy, prefs.attachment_auto_on_wifi, prefs.attachment_auto_on_cellular));
    FeedAttachmentNetwork();
  }
  NotifyPolicyPrefs(prefs);
}

void ConversationsHub::Apply(const NotificationPrefs& prefs) {
  AppLifecycle::SetStayConnected(prefs.stay_connected);
  if (!initialized_ || !messaging_ready_) {
    return;
  }
  // Banners only: the push registration does not follow the setting (P005).
  (void)PushDeviceCoordinator::Register(*this);
}

void ConversationsHub::SuspendMeshColdPeers() {
  SyncMobileEphemeralListen();
}

void ConversationsHub::OnAppBackground() {
  if (messaging_ready_) {
    SuspendMeshColdPeers();
  }
}

void ConversationsHub::OnAppForeground() {
  // #416: iOS can leave the mesh's UDP socket dead across a suspend; the mesh reopens it.
  if (mesh_ready_ && mesh_) {
    mesh_->EnsureUdpSocketAlive(mesh::UdpSocketCheckReason::Requested);
  }
  // The open chat's peer is the one the user is about to talk to: warm its link.
  if (!messaging_ready_ || !mesh_messaging_ || !inbox_) {
    return;
  }
  if (const std::string active = inbox_->ActiveThreadId(); !active.empty()) {
    mesh_messaging_->WarmPeerForThread(active);
  }
}

void ConversationsHub::FlushForExit() {
  if (!initialized_) {
    return;
  }
  store_->Flush();
  contacts_->Flush();
  if (messaging_ready_) {
    identity_->Flush();
  }
}

void ConversationsHub::Shutdown() {
  if (!initialized_) {
    return;
  }

  shutdown_requested_.store(true, std::memory_order_release);
  // The unknown-network timer (and its UI post) of this run never reach the hub again; a later run arms its own.
  attachment_network_lifetime_.InvalidateAndWait();
  attachment_network_timer_armed_.store(false);
  {
    std::lock_guard lock(link_mu_);
    ++link_grace_gen_;
    link_grace_over_ = false;
  }
  link_grace_lifetime_.InvalidateAndWait();        // the status line's Unknown timer of this run
  storage_callbacks_lifetime_.InvalidateAndWait(); // the attachment change reports of this run (rewired at Initialize)
  transfer_lane_.Shutdown(); // aborts a transfer at its next progress tick and joins both lane threads
  AppLifecycle::RemoveListener(lifecycle_background_id_);
  AppLifecycle::RemoveListener(lifecycle_foreground_id_);
  lifecycle_background_id_ = 0;
  lifecycle_foreground_id_ = 0;

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
    mesh_messaging_->SetOnBriefRelayHealthChanged(nullptr);
    mesh_messaging_->SetOnMessagesChanged(nullptr);
    mesh_messaging_->SetOnDeliveryNotice(nullptr);
    mesh_messaging_->SetOnBackgroundUnread(nullptr);
    mesh_messaging_->SetGroupMembership(nullptr);
    mesh_messaging_->SetOnSiblingNotice(nullptr);
    mesh_messaging_->OwnAccountCards().SetOnDeviceJoined(nullptr);
  }
  if (inbox_) {
    inbox_->SetGroupMembership(nullptr);
  }
  for (HubParticipant* participant : ParticipantsDown()) {
    participant->OnTeardownStarting();
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
    for (IDekConsumer* at_rest : at_rest_) {
      secrets_->UnregisterDekConsumer(at_rest);
    }
    for (HubParticipant* participant : ParticipantsDown()) {
      for (IDekConsumer* consumer : participant->DekConsumers()) {
        secrets_->UnregisterDekConsumer(consumer);
      }
    }
  }
  // Stop mesh (joins MeshPump) before dropping session façade.
  StopMesh();
  // Participants let go of messaging before it goes (the call sessions deliver through it).
  for (HubParticipant* participant : ParticipantsDown()) {
    participant->OnMessagingDropped();
  }
  // Destroy P2P before groups — P2P held a non-owning Groups pointer.
  mesh_messaging_.reset();
  // Nothing receives any more: the pricing families may let go of their stores.
  system_controls_.Unbind(kInitiationBillingFamily);
  system_controls_.Unbind(kPaymentPromiseFamily);
  system_controls_.Unbind(kGroupRosterSyncFamily);
  system_controls_.Unbind(kCopyAskFamily);
  system_controls_.Unbind(kCardAskFamily);
  system_controls_.Unbind(kGroupLinkJoinFamily);
  account_locker_.reset();
  group_links_.reset();
  group_membership_.reset();
  group_invite_gate_.reset();
  group_roster_.reset();
  invite_links_.reset();
  thread_prefs_.reset();
  if (inbox_) {
    inbox_->SetThreadMentions(nullptr);
  }
  thread_mentions_.reset();
  contact_cards_.reset();
  contact_profile_refresh_.reset();
  account_keys_.reset();
  peer_cards_.reset();
  peer_card_rows_.reset();
  if (attachment_downloads_) {
    attachment_downloads_->SetContent(nullptr); // a worker still running keeps its own reference
  }
  content_.reset();
  // Participants free the rest (media engine, key and session stores) after DEK unregister above.
  for (HubParticipant* participant : ParticipantsDown()) {
    participant->OnShutdown();
  }
  if (mesh_owner_) {
    mesh_owner_->ClearMeshObjects();
  }
  psk_store_.reset();
  control_lane_seen_.reset();
  inbox_.reset();
  peer_labels_.reset();
  directory_shadows_.reset();
  if (mesh_owner_) {
    mesh_owner_->StopDirectoryCache();
  }
  clients_.Clear();
  relay_ = nullptr;
  push_devices_ = nullptr;
  client_compat_ = nullptr;
  directory_.SetBackend(nullptr);
  registration_ = nullptr;
  blob_ = nullptr;
  identity_.reset();
  if (contacts_) {
    contacts_->SetOnChanged({});
  }
  contacts_.reset();
  store_.reset();
  at_rest_.clear();
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

IThreadLocalPrefsStore& ConversationsHub::ThreadPrefs() {
  return *thread_prefs_;
}

IThreadMentionsStore* ConversationsHub::ThreadMentions() {
  return thread_mentions_.get();
}

MeshDeliveryOrchestrator& ConversationsHub::MeshMessaging() {
  return *mesh_messaging_;
}

GroupMembershipWorkflow& ConversationsHub::Groups() {
  return *group_membership_;
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

IPeerSessionStore* ConversationsHub::PeerSessions() {
  return psk_store_.get();
}

ProfileSecretsEngine* ConversationsHub::Secrets() {
  return secrets_;
}

ContactCardExchange& ConversationsHub::ContactCards() {
  return *contact_cards_;
}

Roe<OwnDeviceList> ConversationsHub::OwnDevices() {
  if (!IsMessagingReady() || !mesh_messaging_) {
    return Error("Messaging is not ready");
  }
  return mesh_messaging_->OwnAccountCards().Devices();
}

Roe<void> ConversationsHub::RemoveOwnDevice(const std::string& peer_id) {
  if (!IsMessagingReady() || !mesh_messaging_) {
    return Error("Messaging is not ready");
  }
  auto card = mesh_messaging_->OwnAccountCards().RemoveDevice(peer_id, util::NowUnixMs());
  if (!card) {
    return card.error();
  }
  // Siblings hear through our own mailbox (OwnCards::SetOnChanged) — the removed device too, which then stops;
  // contacts stop sending to it once they hold this card (M021 limit 4: eventual).
  log().info << "Device " << peer_id << " taken off the account; " << card->devices.size() << " left";
  AppRuntime::PostWorkerNormal([this]() { SyncLockerThenAnnounceOwnDevices(); });
  return {};
}

Roe<void> ConversationsHub::MakeMainDevice(const std::string& peer_id) {
  if (!IsMessagingReady() || !mesh_messaging_) {
    return Error("Messaging is not ready");
  }
  auto card = mesh_messaging_->OwnAccountCards().MakeMaster(peer_id, util::NowUnixMs());
  if (!card) {
    return card.error();
  }
  log().info << "Main device handed to " << peer_id;
  AppRuntime::PostWorkerNormal([this]() { SyncLockerThenAnnounceOwnDevices(); });
  return {};
}

void ConversationsHub::SyncLockerThenAnnounceOwnDevices() {
  // The address book first: a device that just became the master may not have synced its contacts yet (its
  // first sync runs a few seconds after start), and the announce goes to them.
  if (account_locker_) {
    if (auto seed = Identity().RecoverySeed(); seed && *seed) {
      if (auto published = account_locker_->Publish(); !published) {
        log().info << "Account locker not synced before the device announce: " << published.error().message;
      }
    }
  }
  AnnounceOwnDevicesToContacts();
}

void ConversationsHub::ExpectDeviceJoin(const int64_t until_ms) {
  if (mesh_messaging_) {
    mesh_messaging_->OwnAccountCards().ExpectJoin(until_ms);
  }
}

std::optional<std::string> ConversationsHub::OwnAccountCardJson() {
  if (!mesh_messaging_) {
    return std::nullopt;
  }
  auto card = mesh_messaging_->OwnAccountCards().Card(util::NowUnixMs());
  if (!card) {
    log().warning << "own account card unavailable: " << card.error().message;
    return std::nullopt;
  }
  return DumpJson(AccountCardToJson(*card));
}

Roe<void> ConversationsHub::LearnLinkedAccountCard(const std::string& card_json) {
  auto learned = KeepLinkedAccountCard(card_json);
  if (!learned) {
    log().warning << "link import: account card not kept: " << learned.error().message;
  }
  return learned;
}

Roe<void> ConversationsHub::KeepLinkedAccountCard(const std::string& card_json) {
  auto json = TryParseObject(card_json);
  if (!json) {
    return Error("link bundle: account card is not JSON");
  }
  auto card = AccountCardFromJson(*json);
  if (!card) {
    return card.error();
  }
  const int64_t now = util::NowUnixMs();
  if (mesh_messaging_) {
    auto learned = mesh_messaging_->OwnAccountCards().Learn(*card, now);
    if (!learned) {
      return learned.error();
    }
    return {};
  }
  if (!account_keys_) {
    return Error("Messaging is not ready");
  }
  // Kept now; this device signs itself into it once its own card is first asked for (OwnCards::Card).
  auto kept = account_keys_->AddAccountCard(*card, now, "sibling");
  if (!kept) {
    return kept.error();
  }
  return {};
}

Roe<std::string> ConversationsHub::OwnContactLocator() {
  auto identity = Identity().Get();
  if (!identity || identity->account_id.empty()) {
    return Error("Account not set up");
  }
  ContactLocator locator{.account_id = identity->account_id, .nickname = identity->nickname};
  if (auto self = DevicePeerIdFromKey(identity->public_key_b64)) {
    locator.devices.push_back(*self);
  }
  if (account_keys_) {
    if (auto kept = account_keys_->Kept(identity->account_id); kept && *kept) {
      for (const std::string& peer_id : (*kept)->devices) {
        if (std::find(locator.devices.begin(), locator.devices.end(), peer_id) == locator.devices.end()) {
          locator.devices.push_back(peer_id);
        }
      }
    }
  }
  return EncodeContactLocator(locator);
}

Roe<ThreadMessage> ConversationsHub::ShareContact(const std::string& thread_id, const std::string& contact_id) {
  if (!IsInitialized() || !contact_cards_ || !mesh_messaging_) {
    return Error("Messaging not ready");
  }
  auto card = contact_cards_->CardFor(contact_id);
  if (!card) {
    return card.error();
  }
  auto thread = store_->GetThread(thread_id);
  if (!thread || !*thread) {
    return Error("Thread not found");
  }
  SendRelayOptions opts;
  opts.content_type = ChatContentType::ContactCard;
  opts.payload_json = ChatPayloadCodec::ContactCardFieldsToJson(*card);
  if ((*thread)->kind == ThreadKind::Group) {
    return mesh_messaging_->SendGroupMessage(thread_id, card->display_name, opts);
  }
  return mesh_messaging_->SendUserMessage(thread_id, card->display_name, opts);
}

mesh::IDirectoryClient& ConversationsHub::Directory() {
  return directory_;
}

DirectoryShadowCache& ConversationsHub::DirectoryShadows() {
  return *directory_shadows_;
}

PeerDisplayResolver& ConversationsHub::PeerLabels() {
  return *peer_labels_;
}

namespace {

// Registration() / Blob() hand out references, but a build with no backend address creates neither client (the UI
// demo app, or a custom backend left blank). These stand in so a call reports an error instead of dereferencing null.
class UnconfiguredRegistrationClient final : public IRegistrationClient {
public:
  Roe<RegistrationStartResult> StartRegistration(const std::string&, const std::string&, const std::string&,
                                                 const std::string&, const std::string&,
                                                 const std::vector<std::string>&,
                                                 const RegistrationPublishOpts&) override {
    return Unavailable();
  }
  Roe<RegistrationResult> FinishRegistration(const std::string&, const std::string&, const std::string&,
                                             const std::string&, int64_t, const std::string&, const std::string&,
                                             const std::string&, const std::string&, const std::vector<std::string>&,
                                             int64_t, const RegistrationPublishOpts&) override {
    return Unavailable();
  }
  Roe<RegistrationResult> UpdateNickname(const std::string&, const std::string&, int64_t, const std::string&) override {
    return Unavailable();
  }
  Roe<void> RevokeRegistration(const std::string&, const std::string&, int64_t) override { return Unavailable(); }

private:
  static Error Unavailable() { return Error("registration not configured"); }
};

class UnconfiguredBlobClient final : public IBlobClient {
public:
  Roe<BlobPresignResult> Presign(const std::string&, const std::string&, uint64_t, BlobPurpose) override {
    return Unavailable();
  }
  Roe<void> Retain(const std::string&, const std::string&) override { return Unavailable(); }
  Roe<void> Delete(const std::string&, const std::string&) override { return Unavailable(); }
  Roe<void> SetProfileIcon(const std::string&, const std::string&, const std::string&, const std::string&) override {
    return Unavailable();
  }
  Roe<void> PutUpload(const std::string&, const std::string&, const std::string&) override { return Unavailable(); }
  Roe<RelayFileStartResult> FileStart(const std::string&, uint64_t) override { return Unavailable(); }
  Roe<RelayFilePartResult> FilePart(const std::string&, const std::string&, uint64_t) override { return Unavailable(); }
  Roe<RelayFileCompleteResult> FileComplete(const std::string&, const std::string&) override { return Unavailable(); }
  Roe<void> FileDelete(const std::string&, const std::string&) override { return Unavailable(); }
  Roe<RelayFileListResult> FileList(const std::string&, const std::string&) override { return Unavailable(); }

private:
  static Error Unavailable() { return Error("blob service not configured"); }
};

} // namespace

IRegistrationClient& ConversationsHub::Registration() {
  static UnconfiguredRegistrationClient unconfigured;
  return registration_ ? *registration_ : unconfigured;
}

IBlobClient& ConversationsHub::Blob() {
  static UnconfiguredBlobClient unconfigured;
  return blob_ ? *blob_ : unconfigured;
}

IPushDeviceClient* ConversationsHub::PushDevices() {
  return push_devices_;
}

IClientCompatClient* ConversationsHub::ClientCompat() {
  return client_compat_;
}

} // namespace pbr
