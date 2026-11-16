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
#include "feature/conversations/ContactRefreshWorkflow.h"
#include "lib/messaging/ContactLocator.h"
#include "lib/delivery/PeerRequestAuth.h"
#include "lib/people/PeerCards.h"

#include "feature/conversations/PushDeviceCoordinator.h"
#include "lib/people/RegistrationStatus.h"
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
#include "lib/net/OrgBackendClientsImpl.h"
#include "lib/net/ProfileIconClient.h"
#include "lib/net/ProfileIconFetch.h"
#include "domain/registration/RegistrationClient.h"
#include "lib/net/RelayOrigins.h"
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
#include <future>
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

void FillRegistrationFields(ProfileIdentityView& view, const LocalIdentity& identity, const std::string& backend_url) {
  const RegistrationStatus status = ClassifyRegistration(identity, 0, backend_url);
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
  // A name search asks our other relays' directories on workers while it asks the first itself (multi-relay R2).
  clients_.SetDirectoryWorker([](std::function<void()> task) {
    if (!AppRuntime::IsRunning()) {
      task(); // no pool (a test, start-up): in turn
      return;
    }
    AppRuntime::PostWorkerNormal(std::move(task));
  });
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
      if (auto told = mesh_->Amp()->Links().RemoteCapability(peer_id)) {
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
  clients_.Apply({.relay = settings.relay,
                  .directory = settings.directory,
                  .registration = settings.registration,
                  .our_relays = our_relays_ ? our_relays_->List().Origins() : std::vector<std::string>{},
                  .extra_relays = settings.relay_extra_base_urls});
  relay_ = clients_.Relay();
  {
    std::lock_guard lock(clients_mu_);
    push_devices_ = clients_.PushDevices();
    client_compat_ = clients_.ClientCompat();
    registration_ = clients_.Registration();
    blob_ = clients_.Blob();
  }
  if (account_locker_) {
    account_locker_->SetClient(clients_.Locker());
  }
  ExpectRelayIdentities(settings); // before the card lists them: each relay's address names its PeerId (MR008)
  (void)SetCardRelays();
  WireOurRelays();
  CheckRelayIdentitiesSoon();
}

void ConversationsHub::ExpectRelayIdentities(const HubSettings& settings) {
  const std::shared_ptr<RelayPins> pins = RelayIdentityPins();
  if (!pins) {
    return;
  }
  // The org's first (the build's or the config's anchor), then ours: a relay of ours naming the org's PeerId at
  // another address is refused, not the org (MR008).
  const auto expect = [&pins](const std::string& origin, const std::string& peer_id) {
    if (!origin.empty() && !peer_id.empty()) {
      (void)pins->Expect(origin, peer_id); // a conflict is logged and leaves that relay unused
    }
  };
  const std::string home_peer_id =
      settings.registration.peer_id.empty() && SameRelay(clients_.LockerHome(), clients_.OrgRelay())
          ? settings.relay.peer_id
          : settings.registration.peer_id;
  expect(clients_.OrgRelay(), settings.relay.peer_id);
  expect(clients_.LockerHome(), home_peer_id);
  NoteOrgRelayWithoutId(settings, clients_.OrgRelay(), settings.relay.peer_id, "relay.peer_id");
  NoteOrgRelayWithoutId(settings, clients_.LockerHome(), home_peer_id, "registration.peer_id");
  const auto named_by_org = [&](const std::string& origin) {
    return (!settings.relay.peer_id.empty() && SameRelay(origin, clients_.OrgRelay())) ||
           (!home_peer_id.empty() && SameRelay(origin, clients_.LockerHome()));
  };
  if (our_relays_) {
    for (const OurRelayEntry& entry : our_relays_->List().relays) {
      if (!named_by_org(entry.origin)) {
        expect(entry.origin, entry.peer_id); // the org's relay on our list keeps the build's PeerId (a newer build's)
      }
    }
  }
}

bool ConversationsHub::SetCardRelays() {
  // Our card lists the relays we register on (multi-relay MR002), preferred first, each with its pinned PeerId
  // (MR007: a relay not pinned yet, or whose identity changed, is left off); OwnCards keeps the https ones.
  const auto pins = RelayIdentityPins();
  if (!mesh_messaging_ || !pins) {
    return false;
  }
  return mesh_messaging_->OwnAccountCards().SetRelays(pins->PinnedRelays(OurRelayOrigins()));
}

void ConversationsHub::OpenRelayPins() {
  CloseRelayPins();
  auto pins = std::make_shared<RelayPins>(data_dir_);
  pins->SetKeep([this](const std::string& origin) { return KeepsPin(origin); });
  pins->SetFirstUse([this](const std::string& origin) { return FirstUseAllowed(origin); });
  pins->SetOnChanged([this](const std::string&) { OnRelayPinsChanged(); });
  pins->SetOnNotRegistered([this](const std::string& origin) { HealRegistrationSoon(origin); });
  {
    std::lock_guard lock(relay_pins_mu_);
    relay_pins_ = pins;
  }
  clients_.SetPins(std::move(pins)); // before the clients are built: each signs for its relay's pin
}

void ConversationsHub::CloseRelayPins() {
  std::shared_ptr<RelayPins> closing;
  {
    std::lock_guard lock(relay_pins_mu_);
    closing = std::move(relay_pins_);
  }
  if (!closing) {
    return;
  }
  closing->SetKeep({});
  closing->SetFirstUse({});
  closing->SetOnChanged({});
  closing->SetOnNotRegistered({});
}

bool ConversationsHub::KeepsPin(const std::string& origin) {
  // Ours and the org's origins (a relay not ours is never pinned, MR007).
  std::lock_guard lock(kept_pins_mu_);
  return std::any_of(kept_pins_.begin(), kept_pins_.end(),
                     [&origin](const std::string& kept) { return SameRelay(kept, origin); });
}

void ConversationsHub::NoteOrgRelayWithoutId(const HubSettings& settings, const std::string& origin,
                                             const std::string& peer_id, const char* setting) {
  if (origin.empty() || !peer_id.empty() || settings.org_first_use) {
    return;
  }
  // MR008 item 4: a release build takes no relay on its first answer, the org's neither (Me → Network says so).
  log().error << "the backend's relay at " << origin << " names no relay ID: it is not used (set " << setting
              << " to the PeerId after # in the relay's link)";
}

bool ConversationsHub::FirstUseAllowed(const std::string& origin) {
  // MR008 item 4: a developer build may take a bare origin of ours on its first answer; the org's origins too in a
  // build that names no PeerId for its compiled backend — a dogfood build, logged as a warning when it happens. A
  // release build's custom backend (or PP_BROWSER_ORIGIN) without one: never.
  if (config_.developer_relays) {
    return KeepsPin(origin);
  }
  if (!config_.org_first_use) {
    return false;
  }
  std::lock_guard lock(kept_pins_mu_);
  return std::any_of(org_origins_.begin(), org_origins_.end(),
                     [&origin](const std::string& org) { return SameRelay(org, origin); });
}

void ConversationsHub::OnRelayPinsChanged() {
  AppRuntime::PostUI([this]() {
    if (shutdown_requested_.load(std::memory_order_acquire) || !IsMessagingReady()) {
      return;
    }
    // While a pass checks our relays, the card waits for its end: one card with every relay that answers to its
    // address, not one per check, which a linked device would follow and then follow back.
    if (checking_passes_.load(std::memory_order_acquire) == 0 && SetCardRelays() && IsMainDevice()) {
      // A relay pinned now (an Add) or one that does not match its address: our card lists it, or leaves it off.
      AppRuntime::PostWorkerNormal([this]() { ReissueAndAnnounceCard(); });
    }
    NotifyOurRelaysChanged();
  });
}

void ConversationsHub::CheckRelayIdentitiesSoon() {
  std::vector<std::string> origins = OurRelayOrigins();
  std::vector<std::string> org;
  for (const std::string& origin : {clients_.OrgRelay(), clients_.LockerHome()}) {
    if (!origin.empty()) {
      origins.push_back(origin); // the org's origins are checked whether or not they are ours (lockers, the LLM token)
      org.push_back(origin);
    }
  }
  {
    std::lock_guard lock(kept_pins_mu_);
    kept_pins_ = origins;
    org_origins_ = std::move(org);
  }
  const std::shared_ptr<RelayPins> pins = RelayIdentityPins();
  if (!pins) {
    return;
  }
  checking_passes_.fetch_add(1, std::memory_order_acq_rel);
  AppRuntime::PostWorkerNormal([this, pins, origins = std::move(origins)]() {
    if (!shutdown_requested_.load(std::memory_order_acquire)) {
      pins->CheckIdentities(origins); // each change reaches OnRelayPinsChanged (the page; the card waits)
    }
    if (checking_passes_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      OnRelayPinsChanged(); // the pass is over: the card takes what it found, once
    }
  });
}

void ConversationsHub::RecheckRelayIdentitiesSoon() {
  AppRuntime::PostUI([this]() {
    if (!shutdown_requested_.load(std::memory_order_acquire) && IsMessagingReady()) {
      CheckRelayIdentitiesSoon(); // on the hub's thread: it reads the clients
    }
  });
}

void ConversationsHub::HealRegistrationSoon(const std::string& origin) {
  AppRuntime::PostWorkerNormal([this, origin]() {
    if (shutdown_requested_.load(std::memory_order_acquire) || !IsMessagingReady()) {
      return;
    }
    const std::vector<std::string> ours = OurRelayOrigins();
    if (!ours.empty() && SameRelay(origin, ours.front())) {
      HealPreferredRegistration(util::NowUnixMs());
      return;
    }
    // Another relay of ours (a relay not ours, the org's origin alone, is not healed: nothing of ours is there).
    if (const auto registrations = OtherRelayRegistrations();
        registrations && registrations->Lost(origin, util::NowUnixMs())) {
      RegisterOnOtherRelaysSoon();
    }
  });
}

void ConversationsHub::HealPreferredRegistration(const int64_t now_ms) {
  auto identity = Identity().Get();
  const std::shared_ptr<IRegistrationClient> registration = Registration();
  // Only a registration we believe current: registered there and not expired (an expired one renews as it does).
  const RegistrationStatus status =
      identity ? ClassifyRegistration(*identity, now_ms, registration->BaseUrl()) : RegistrationStatus::Unregistered;
  if (status != RegistrationStatus::Active && status != RegistrationStatus::ExpiringSoon) {
    return;
  }
  {
    std::lock_guard lock(preferred_loss_mu_);
    if (preferred_heal_due_ms_ == 0) {
      preferred_heal_due_ms_ = preferred_loss_.Lost(now_ms);
    }
    if (now_ms < preferred_heal_due_ms_) {
      return; // lost again soon after the last heal: after its backoff
    }
    preferred_heal_due_ms_ = 0;
  }
  log().warning << "the preferred relay no longer holds our registration; registering again";
  // Whatever the auto-renew preference: the person registered there already (MR007 self-heal).
  auto renewed = FinishAndPersistRegistration(*registration, Identity(), identity->nickname, {}, RegistrationCard(),
                                              PreferredRegistrationHome());
  if (!renewed) {
    log().warning << "registering again on the preferred relay failed: " << renewed.error().message;
    return;
  }
  (void)PushDeviceCoordinator::Register(*this); // push binds again; polling resumes on its own
}

void ConversationsHub::ReissueAndAnnounceCard() {
  if (shutdown_requested_.load(std::memory_order_acquire) || !mesh_messaging_ || !IsMessagingReady()) {
    return; // the next start issues the card (and the registrations carry it)
  }
  // Issued now: siblings hear (OwnCards::SetOnChanged), then our contacts get it, and our relays take it.
  if (auto card = mesh_messaging_->OwnAccountCards().Card(util::NowUnixMs()); !card) {
    log().warning << "account card not re-issued: " << card.error().message;
    return;
  }
  AnnounceOwnDevicesToContacts();
  RegisterOnOtherRelaysSoon();
  if (auto published = MaybeAutoRenewRegistration(*Registration(), Identity(), false, RegistrationCard(),
                                                  PreferredRegistrationHome());
      !published) {
    log().info << "the re-issued card did not reach the preferred relay yet: " << published.error().message;
  }
}

void ConversationsHub::WireOurRelays() {
  std::vector<RelayInbox> inboxes;
  std::vector<RelayRegistrationsWorkflow::Relay> others;
  const OurRelaysList kept = our_relays_ ? our_relays_->List() : OurRelaysList{};
  for (const BackendClientSet::OurRelay& relay : clients_.OurRelays()) {
    inboxes.push_back(RelayInbox{.origin = relay.origin, .client = relay.relay});
    if (relay.registration) {
      RelayRegistrationRecord last;
      for (const OurRelayEntry& entry : kept.relays) {
        if (SameRelay(entry.origin, relay.origin)) {
          last = entry.registration; // what it answered before this start
        }
      }
      others.push_back({.origin = relay.origin, .client = relay.registration, .last = std::move(last)});
    }
  }
  if (const auto registrations = OtherRelayRegistrations()) {
    registrations->SetRelays(std::move(others));
  }
  if (contact_refresh_) {
    contact_refresh_->SetOurRelays(CardDirectories()); // the Refresh asks them after the contact's own relays
  }
  if (!mesh_messaging_) {
    return;
  }
  mesh_messaging_->SetCardDirectories(CardDirectories());
  mesh_messaging_->SetRelayClients({.preferred = relay_,
                                    .history = clients_.HistoryRelay(),
                                    .sender = &clients_.Sender(),
                                    .inboxes = std::move(inboxes)});
}

std::vector<std::string> ConversationsHub::CardDirectories() const {
  // Our relays' directories, then the other directories we search (http ones: the mesh's are its own).
  std::vector<std::string> directories = OurRelayOrigins();
  for (const ServiceEndpointConfig& provider : EffectiveDirectoryProviders(config_.directory)) {
    const bool http = provider.transport.empty() || provider.transport == "http";
    const bool listed = std::any_of(directories.begin(), directories.end(), [&provider](const std::string& known) {
      return SameRelay(known, provider.base_url);
    });
    if (http && !provider.base_url.empty() && !listed) {
      directories.push_back(provider.base_url);
    }
  }
  return directories;
}

void ConversationsHub::SetOnOurRelaysChanged(std::function<void()> callback) {
  std::lock_guard lock(our_relays_changed_mu_);
  on_our_relays_changed_ = std::move(callback);
}

void ConversationsHub::NotifyOurRelaysChanged() {
  std::function<void()> callback;
  {
    std::lock_guard lock(our_relays_changed_mu_);
    callback = on_our_relays_changed_;
  }
  if (callback) {
    callback();
  }
}

void ConversationsHub::RegisterOnOtherRelaysSoon() {
  AppRuntime::PostWorkerNormal([this]() {
    // A copy: Shutdown may take the workflow meanwhile; once it is closed this run does nothing.
    const auto registrations = OtherRelayRegistrations();
    if (!registrations || !IsMessagingReady() || shutdown_requested_.load(std::memory_order_acquire)) {
      return;
    }
    registrations->RunDue(util::NowUnixMs());
  });
}

std::vector<std::string> ConversationsHub::OurRelayOrigins() const {
  std::vector<std::string> origins;
  for (const BackendClientSet::OurRelay& relay : clients_.OurRelays()) {
    origins.push_back(relay.origin);
  }
  return origins;
}

std::shared_ptr<IRelayClient> ConversationsHub::OurRelayClient(const std::string& origin) const {
  for (const BackendClientSet::OurRelay& relay : clients_.OurRelays()) {
    if (SameRelay(relay.origin, origin)) {
      return relay.relay;
    }
  }
  return nullptr;
}

std::vector<std::pair<std::string, std::shared_ptr<IPushDeviceClient>>> ConversationsHub::OtherRelayPushDevices() {
  std::vector<std::pair<std::string, std::shared_ptr<IPushDeviceClient>>> out;
  const auto registrations = OtherRelayRegistrations();
  for (const BackendClientSet::OurRelay& relay : clients_.OurRelays()) {
    if (relay.registration && relay.push_devices && registrations && registrations->Registered(relay.origin)) {
      out.emplace_back(relay.origin, relay.push_devices); // shared: a binding in flight keeps it
    }
  }
  return out;
}

void ConversationsHub::InstallOrgBackendClients(const HubSettings& settings) {
  clients_.SetLocalRelaysAllowed(settings.developer_relays); // a relay the person added: public addresses otherwise
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
      if (linked) {
        RecheckRelayIdentitiesSoon(); // a relay not checked while offline is asked now, not after its backoff
        RetryOwnCardRecoverySoon();   // and a card of ours still to get back
      }
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
    mesh_messaging_->SetOnRelayNotRegistered(nullptr);
    InvalidateRelayRegistrationSignals();
    mesh_messaging_->SetOnMessagesChanged(nullptr);
    mesh_messaging_->SetOnDeliveryNotice(nullptr);
    mesh_messaging_->SetOnBackgroundUnread(nullptr);
    mesh_messaging_->SetGroupMembership(nullptr);
    mesh_messaging_->SetOnSiblingNotice(nullptr);
    mesh_messaging_->OwnAccountCards().SetOnDeviceJoined(nullptr);
    mesh_messaging_->SetOnInboxPolled(nullptr);
    mesh_messaging_->SetOnOwnCardChanged(nullptr);
    mesh_messaging_->SetAttachmentDownloads(nullptr);
  }
  if (inbox_) {
    inbox_->SetGroupMembership(nullptr);
    inbox_->SetAttachmentDownloads(nullptr);
    inbox_->SetCancelPendingSend(nullptr);
  }
  system_controls_.Unbind(kGroupRosterSyncFamily);
  system_controls_.Unbind(kCopyAskFamily);
  system_controls_.Unbind(kCardAskFamily);
  system_controls_.Unbind(kGroupLinkJoinFamily);
  system_controls_.Unbind(relay_grant::kFamily);
  clients_.SetRelaySendHooks({});
  clients_.Sender().SetRoutes({});
  if (peer_cards_) {
    peer_cards_->SetOnDevicesAdded(nullptr);
  }
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

void ConversationsHub::OpenOurRelays(const HubSettings& settings) {
  // Before My relays (its ports read them) and the clients (each signs for its relay's pin, MR007).
  checking_passes_.store(0, std::memory_order_release); // a reset: a pass dropped at Shutdown holds no card back
  OpenRelayPins();
  // My relays (multi-relay R3, MR006): the profile's list, seeded once from the deployment's relay and config.
  CloseMyRelays(); // a profile reset: the old workflow (closed at Shutdown) goes before the store it changes
  our_relays_ = std::make_unique<OurRelaysStore>(data_dir_);
  our_relays_->LoadOrSeed(clients_.OrgRelayOf({.relay = settings.relay}), settings.relay.peer_id,
                          settings.relay_extra_base_urls, settings.developer_relays);
  CloseRelayRegistrations(); // a reset: closed at Shutdown already
  auto registrations = std::make_shared<RelayRegistrationsWorkflow>(*identity_);
  registrations->SetCard([this]() { return RegistrationCard(); });
  registrations->SetOnResult([this](const std::string& origin, const RelayRegistrationRecord& record) {
    if (our_relays_) {
      our_relays_->Record(origin, record);
    }
    NotifyOurRelaysChanged();
  });
  {
    std::lock_guard lock(my_relays_mu_);
    my_relays_ = std::make_shared<MyRelaysWorkflow>(*our_relays_, MyRelaysPorts());
  }
  // A relay that took our registration takes our push token too (any relay holding mail can wake the phone).
  registrations->SetOnRegistered(
      [this](const std::string& origin) { (void)PushDeviceCoordinator::RegisterOn(*this, origin); });
  {
    std::lock_guard lock(relay_registrations_mu_);
    relay_registrations_ = std::move(registrations);
  }
}

void ConversationsHub::OpenContactCards() {
  contact_cards_ = std::make_unique<ContactCardExchange>(*contacts_, *account_keys_);
  contact_cards_->SetLocalRelaysAllowed(config_.developer_relays); // a QR code's relay not on a public host: the lab's
  contact_profile_refresh_ = std::make_unique<ContactProfileRefresh>(*contacts_, directory_);
  contact_refresh_ = std::make_unique<ContactRefreshWorkflow>(*contacts_, *account_keys_, *contact_cards_);
  contact_refresh_->SetOurRelays(CardDirectories());
  // The page's *identity doesn't match* (MR007): our pins disagree with the card, or a send found the PeerId refused.
  contact_refresh_->SetIdentityMismatch(
      [this](const std::string& account_id, const std::string& origin, const std::string& peer_id) {
        const auto pins = RelayIdentityPins();
        return (pins && pins->CheckCardRelay(origin, peer_id) == CardRelayCheck::Mismatch) ||
               clients_.Sender().AudienceMismatch(account_id, origin);
      });
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
  if (opened_stores->set_own_account) {
    opened_stores->set_own_account([this]() { return OwnAccountId(); }); // profile.db's steps tell our card by it
  }
  store_ = std::move(opened_stores->threads);
  psk_store_ = std::move(opened_stores->peer_sessions);
  control_lane_seen_ = std::move(opened_stores->control_lane_seen);
  relay_grants_ = std::move(opened_stores->relay_grants);
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
    SweepRelayGrantsSoon(); // a contact added gets a grant; one blocked, none renewed
  });
  PublishAddressDisclosure();
  identity_ = std::make_unique<IdentityStore>(data_dir_, profile_id_);
  OpenOurRelays(settings);
  initiation_billing_ = std::make_unique<InitiationBillingStore>(data_dir_);
  (void)initiation_billing_->Load();
  payment_promises_ = std::make_unique<PaymentPromiseStore>(data_dir_);
  (void)payment_promises_->Load();

  InstallOrgBackendClients(settings);

  // The account locker keeps every group (G013): republish it on any roster change.
  group_roster_->SetOnChanged([this](const GroupRosterChange& change) {
    PublishAccountLockerSoon();
    if (!change.member_gone.empty() && mesh_messaging_ && grant_peers_ && !grant_peers_->Granted(change.member_gone)) {
      mesh_messaging_->RelayGrants().Stop(change.member_gone); // removed, and no other reason to grant them
    }
    SweepRelayGrantsSoon(); // a member who joined gets a grant
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
    // The entry's floor counts only while no card of the account is known (its card's speaks over it, MR005).
    const std::string person_id = PrimaryAccountIdFromHit(hit).value_or(std::string());
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
    // Initialized while the vault is open (Reset chats): the new stores get the key now, so the chat store opens.
    if (secrets_->IsUnlocked()) {
      (void)secrets_->RedistributeUnlockedDek();
    }
  }
  BindPricingFamilies();
  peer_cards_ = std::make_unique<PeerCards>(*peer_card_rows_);
  // The account locker keeps how new each account's newest card is (a restored device refuses older ones).
  peer_cards_->SetOnNewerCard([this]() { PublishAccountLockerSoon(); });
  account_keys_ = std::make_unique<AccountKeyResolver>(signing_key_store_, kem_key_store_, *peer_cards_, directory_);
  account_keys_->SetOwnAccount([this]() { return OwnAccountId(); }); // our own card is our devices' only
  kept_cards_loaded_ = false; // read at the first EnsureMessagingReady (the cards are in the chat store)
  WireCardReaders();
  OpenContactCards();

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

void ConversationsHub::WireCardReaders() {
  // A directory lookup's card is the account's word on its relays and price (multi-relay R2): kept once it verifies.
  // Not ours: our own card is OwnCards'.
  clients_.DirectoryCards().Set([this](const std::string& account_id, const Object& card) {
    auto identity = identity_ ? identity_->Get() : Roe<LocalIdentity>(Error("no identity"));
    if (!account_keys_ || (identity && identity->account_id == account_id)) {
      return;
    }
    if (auto kept = account_keys_->AddCarriedCard(card, util::NowUnixMs(), "directory", account_id); !kept) {
      log().debug << "a directory's card for " << account_id << " not kept: " << kept.error().message;
    }
  });
  // A stranger's price is its newest kept card's (MR005), read when the chat or a call decides it.
  initiation_billing_->SetCardFloors([this](const std::string& peer) -> std::optional<InitiationCardFloor> {
    if (!account_keys_ || !IsAccountIdentityValue(peer)) {
      return std::nullopt;
    }
    auto kept = account_keys_->Kept(peer);
    if (!kept || !*kept) {
      return std::nullopt;
    }
    const std::optional<int64_t> floor = KeptInitiationFloor(**kept);
    if (!floor) {
      return std::nullopt; // only a key card known
    }
    return InitiationCardFloor{.floor_minor = *floor, .issued_at = (*kept)->issued_at};
  });
}

void ConversationsHub::UnwireCardReaders() {
  clients_.DirectoryCards().Set(nullptr);
  if (initiation_billing_) {
    initiation_billing_->SetCardFloors(nullptr);
  }
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

AccountLockerWorkflow::Sources ConversationsHub::LockerSources() {
  return AccountLockerWorkflow::Sources{
      .groups = [this]() -> Roe<std::vector<AccountStateGroup>> {
        return group_membership_ ? group_membership_->StateGroups()
                                 : Roe<std::vector<AccountStateGroup>>(std::vector<AccountStateGroup>{});
      },
      .card_floors = [this]() -> Roe<std::vector<PeerCardFloor>> {
        return peer_cards_ ? peer_cards_->NewestKnown() : Roe<std::vector<PeerCardFloor>>(std::vector<PeerCardFloor>{});
      },
      .take_dissolved =
          [this](const std::vector<AccountStateGroup>& groups) {
            if (group_membership_) {
              for (const AccountStateGroup& group : groups) {
                group_membership_->EndDissolvedFromLocker(group);
              }
            }
          },
      .grants = [this]() -> Roe<std::vector<AccountStateGrant>> {
        if (!mesh_messaging_) {
          return std::vector<AccountStateGrant>{};
        }
        auto held = mesh_messaging_->RelayGrants().ReceivedForLocker();
        if (!held) {
          return held.error();
        }
        std::vector<AccountStateGrant> grants;
        for (const RelayGrant& grant : *held) {
          grants.push_back(AccountStateGrant{.grantor_account_id = grant.recipient_account_id,
                                             .grantee_account_id = grant.sender_account_id,
                                             .issued_at = grant.issued_at,
                                             .expires_at = grant.expires_at,
                                             .signature = grant.signature});
        }
        return grants;
      },
      // A sync: kept keys only (it runs under the locker's lock; a grantor not kept yet is taken at the next).
      .take_grants = [this](const std::vector<AccountStateGrant>& grants) { TakeLockerGrants(grants, false); },
      .own_devices = [this]() -> Roe<OwnDeviceList> {
        if (!mesh_messaging_) {
          return Error("Messaging is not ready");
        }
        return mesh_messaging_->OwnAccountCards().Devices();
      },
      .own_system_name = [this]() { return config_.system_device_name; },
      .relays = [this]() { return LockerRelays(); }};
}

void ConversationsHub::WireMyRelaysHooks() {
  // My relays: each poll says whether the relay answered; a linked device follows the main device's list.
  mesh_messaging_->SetOnInboxPolled([this](const std::string& origin, const bool ok) {
    if (const auto relays = MyRelays()) {
      relays->NotePolled(origin, ok, util::NowUnixMs());
    }
  });
  mesh_messaging_->SetOnOwnCardChanged([this](const AccountCard& card) {
    NotifyOwnDevicesChanged();
    std::vector<AccountCardRelay> relays = card.relays;
    std::vector<OwnCardRelay> pins;
    for (const AccountCardRelay& relay : card.relays) {
      pins.push_back(OwnCardRelay{.origin = relay.origin, .peer_id = relay.peer_id});
    }
    AppRuntime::PostWorkerNormal([this, relays = std::move(relays), pins = std::move(pins)]() {
      if (shutdown_requested_.load(std::memory_order_acquire) || IsMainDevice()) {
        return; // the main device's list and pins are its own
      }
      // A linked device takes our own card's word on each relay's identity (MR007), then follows its list.
      if (const auto relay_pins = RelayIdentityPins()) {
        relay_pins->TakeOwnCard(pins);
      }
      if (const auto workflow = MyRelays()) {
        workflow->FollowCard(relays);
      }
    });
  });
}

void ConversationsHub::BuildMessagingObjects() {
  mesh_messaging_ =
      std::make_unique<MeshDeliveryOrchestrator>(*store_, *contacts_, *identity_, relay_, *inbox_, *account_keys_,
                                                 *psk_store_, *group_roster_, group_invite_gate_.get());
  SetCardRelays(); // before the first card is asked for
  // Another relay of ours is polled once it holds our registration.
  mesh_messaging_->SetInboxReady([this](const std::string& origin) {
    const auto registrations = OtherRelayRegistrations();
    return registrations && registrations->Registered(origin);
  });
  mesh_messaging_->SetSystemControls(&system_controls_);
  // The relay's poll results feed the status line (#175); a fresh orchestrator starts with no result.
  mesh_messaging_->SetOnBriefRelayHealthChanged(
      [this](const MeshDeliveryOrchestrator::BriefRelayHealthState health) { OnBriefRelayHealthChanged(health); });
  // The relay does not know this account (B216): the UI decides whether to register again or to ask.
  relay_not_registered_posted_->store(false); // a post a teardown dropped must not hold the next session's back
  InvalidateRelayRegistrationSignals();
  mesh_messaging_->SetOnRelayNotRegistered(
      [this]() { PostRelayRegistrationSignal(RelayRegistrationSignal::NotRegistered); });
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
  WireOurRelays(); // the inboxes keep their cursors in the profile
  mesh_messaging_->SetContent(content_);
  mesh_messaging_->SetControlLaneSeen(control_lane_seen_.get());
  mesh_messaging_->SetAddressDisclosure(&address_disclosure_);
  mesh_messaging_->SetInitiationBillingStore(initiation_billing_.get());
  WireAttachmentDownloads();
  CleanStaleAttachmentFiles(); // once per start: staged sends older than a day, half-finished downloads
  group_membership_ = std::make_unique<GroupMembershipWorkflow>(*store_, *contacts_, *identity_, *group_roster_,
                                                                *group_invite_gate_, *mesh_messaging_, *account_keys_);
  inbox_->SetGroupMembership(group_membership_.get());
  mesh_messaging_->SetGroupMembership(group_membership_.get());
  // A message deleted here takes an unsent edit of ours with it (wired with each orchestrator, cleared with it).
  inbox_->SetCancelPendingSend([this](const std::string& message_id) {
    if (mesh_messaging_) {
      mesh_messaging_->CancelPendingSend(message_id);
    }
  });
  BindGroupRosterSyncFamily();
  BindCopyAskFamily();
  BindCardAskFamily();
  WireRelayGrants();
  group_links_ = std::make_unique<GroupInviteLinkWorkflow>(*group_roster_, invite_links_.get(), *group_membership_,
                                                           *mesh_messaging_, *identity_);
  BindGroupLinkJoinFamily();
  account_locker_ = std::make_unique<AccountLockerWorkflow>(*identity_, *contacts_, LockerSources());
  account_locker_->SetClient(clients_.Locker());
  account_locker_->SetDataDir(data_dir_);
  // The account's devices keep one address book: a write here nudges the others; a nudge from one syncs here.
  account_locker_->SetOnWritten([this]() {
    if (mesh_messaging_) {
      mesh_messaging_->NudgeSiblings();
    }
  });
  mesh_messaging_->SetOnSiblingNotice([this]() { PublishAccountLockerSoon(); });
  // A device just joined our list (we are the master): our contacts hear of it, so their next message reaches it;
  // the locker is read now, so the new device's name is here without waiting for the 10-minute round.
  mesh_messaging_->OwnAccountCards().SetOnDeviceJoined([this](const AccountCard&) {
    AppRuntime::PostWorkerNormal([this]() { AnnounceOwnDevicesToContacts(); });
    PublishAccountLockerSoon();
  });
  WireMyRelaysHooks();
  WireOwnCardRecovery();
  NotifyMessagingBuilt();
  actions_ =
      std::make_unique<ContactActionDispatcher>(*inbox_, *contacts_, *identity_, *store_, group_membership_.get(),
                                                Registration(), mesh_messaging_.get(), &directory_);
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
  // The chat store opened with the data key (its ladder ran then, D105): the first reads of it are here, after
  // unlock. One that could not be opened (newer than this build, a failed upgrade) stops messaging here.
  {
    StartupPhase reconcile_phase("ConversationsHub::ReconcileOutbox");
    if (auto reconciled = store_->ReconcileOutbox(); !reconciled) {
      return reconciled.error();
    }
  }
  if (!kept_cards_loaded_) {
    account_keys_->LoadKeptCards();
    kept_cards_loaded_ = true;
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

  // Our card back into the chat store's kept cards after a reset of it (it stays beside the identity), before anything
  // reads them.
  mesh_messaging_->OwnAccountCards().Reconcile(util::NowUnixMs());
  // Before the first poll: a restored device refuses cards older than the ones it knew.
  ApplyRestoredCardFloors();
  messaging_ready_ = true;
  mesh_ready_ = false;
  StartCoordinatorTimers();
  NotifyMessagingReady();
  ScheduleMeshBringUp();
  AppRuntime::PostWorkerNormal([this]() {
    ResumeGroupRosterChecks();
    ApplyRestoredGrantsThenAnnounce(); // the restored phone's first sends carry its grants
  });
  SweepRelayGrantsSoon(); // the cutover's too: contacts and members from before MR005 get grants on first run
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
  // Mesh UDP drain is MeshHost's MeshPumpThread (on datagrams and the mesh's own deadlines). Policy stays on the 1s timer.
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
  if (relay_grant_timer_id_ == 0) {
    relay_grant_timer_id_ = AppRuntime::ScheduleCoordinatorRepeating(
        std::chrono::milliseconds(kRelayGrantSweepIntervalMs), [this]() { SweepRelayGrantsSoon(); });
  }
  if (relay_registration_timer_id_ == 0) {
    // Our other relays: registered once the preferred one is, again for a newer card or near the expiry.
    relay_registration_timer_id_ = AppRuntime::ScheduleCoordinatorRepeating(
        std::chrono::milliseconds(kRelayRegistrationCheckIntervalMs), [this]() { RegisterOnOtherRelaysSoon(); });
    RegisterOnOtherRelaysSoon();
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
  if (relay_grant_timer_id_ != 0) {
    AppRuntime::CancelCoordinatorTimer(relay_grant_timer_id_);
    relay_grant_timer_id_ = 0;
  }
  if (relay_registration_timer_id_ != 0) {
    AppRuntime::CancelCoordinatorTimer(relay_registration_timer_id_);
    relay_registration_timer_id_ = 0;
  }
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
  if (health == MeshDeliveryOrchestrator::BriefRelayHealthState::Ok) {
    PostRelayRegistrationSignal(RelayRegistrationSignal::PollOk); // the relay knows us (again)
  }
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

bool ConversationsHub::RefreshContactProfileNow(const std::string& contact_id, const bool force) {
  if (!contact_profile_refresh_ || !AppRuntime::IsRunning() || shutdown_requested_.load(std::memory_order_acquire)) {
    return false;
  }
  auto refreshed = contact_profile_refresh_->RefreshIfStale(contact_id, util::NowUnixMs(), force);
  if (refreshed.updated) {
    EnsureContactIconCached(*refreshed.updated); // fetches the new photo and tells the screens when it is there
    // A changed name or relay id shows on an open page even when no photo is fetched.
    AppRuntime::PostUI([this]() { NotifyPeerIconsChanged(); });
  }
  return refreshed.looked_up;
}

void ConversationsHub::RefreshContactProfileIfStale(const std::string& contact_id, const bool force) {
  if (!contact_profile_refresh_ || !HasDirectory() || !AppRuntime::IsRunning() ||
      !contact_profile_refresh_->NeedsRefresh(contact_id, util::NowUnixMs(), force)) {
    return;
  }
  AppRuntime::PostWorkerBackground([this, contact_id, force]() { RefreshContactProfileNow(contact_id, force); });
}

ContactRelayView ConversationsHub::ContactRelay(const Contact& contact) {
  return contact_refresh_ ? contact_refresh_->PreferredRelay(contact) : ContactRelayView{};
}

bool ConversationsHub::RefreshContact(const std::string& contact_id,
                                      std::function<void(const ContactRefreshResult&)> done) {
  if (!contact_refresh_ || !AppRuntime::IsRunning() || shutdown_requested_.load(std::memory_order_acquire)) {
    return false;
  }
  return contact_refresh_->Start(contact_id, [this, done = std::move(done)](ContactRefreshResult result) {
    if (shutdown_requested_.load(std::memory_order_acquire)) {
      return;
    }
    if (result.contact && result.Changed()) {
      if (mesh_messaging_) {
        mesh_messaging_->RegisterContactDirectEndpoints(*result.contact);
      }
      EnsureContactIconCached(*result.contact);
    }
    done(result);
  });
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
  view.account_id = identity->account_id;
  view.contact_locator = OwnContactLocator().valueOr("");
  view.public_key_b64 = identity->account_signing_public_key_b64;
  FillRegistrationFields(view, *identity, Registration()->BaseUrl()); // the preferred relay's (MR006)
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
    auto result = UpdateRegisteredNickname(*Registration(), Identity(), nickname);
    if (!result) {
      return result.error();
    }
  }
  PublishAccountLockerSoon(); // the locker keeps the nickname
  return {};
}

Roe<RegistrationJob> ConversationsHub::BeginRegistration(const std::string& nickname) {
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
  return MakeRegistrationJob(Registration(), Identity(), *identity, std::move(listen_addrs), RegistrationCard());
}

Roe<RegistrationResult> ConversationsHub::RunRegistration(const RegistrationJob& job) {
  return RunRegistrationJob(job);
}

Roe<void> ConversationsHub::FinishRegistration(const RegistrationResult& result, const std::string& account_id) {
  if (!IsInitialized()) {
    return AppError::Pin(Err::Pin::Required, "Messaging hub not initialized");
  }
  if (!IsMessagingReady()) {
    return AppError::Pin(Err::Pin::Required, "Unlock profile PIN to register");
  }
  if (auto applied = PersistRenewal(Identity(), result, account_id, PreferredRegistrationHome()); !applied) {
    return applied.error();
  }
  PublishAccountLockerSoon();  // the locker keeps the relay binding: a phrase restore needs it
  RegisterOnOtherRelaysSoon(); // and our other relays follow the preferred one
  return {};
}

Roe<void> ConversationsHub::RegisterIdentity(const std::string& nickname) {
  auto job = BeginRegistration(nickname);
  if (!job) {
    return job.error();
  }
  auto result = RunRegistration(*job);
  if (!result) {
    return result.error();
  }
  return FinishRegistration(*result, job->account_id);
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
  auto bundle = BuildRecoveryBundleJson(*seed, state.nickname, now_ms);
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
  RestoreOurRelays(state.extras.relays); // before the first card: it lists them, not the deployment's default
  log().info << "Account restored from its recovery phrase: locker version " << (*fetched)->version << ", "
             << state.contacts.size() << " contact(s); " << state.extras.groups.size() << " group(s) and "
             << state.extras.card_floors.size() << " card floor(s) once messaging is up";
  QueueRestoredState(std::move(state.extras));
  return {};
}

AccountStateRelays ConversationsHub::LockerRelays() const {
  AccountStateRelays relays;
  if (our_relays_) {
    const auto pins = RelayIdentityPins();
    for (const OurRelayEntry& entry : our_relays_->List().relays) {
      // Its address: the PeerId we expect of it (the build's for the org's relay), else the one the list kept.
      std::string peer_id = pins ? pins->PinnedPeerId(entry.origin).value_or(entry.peer_id) : entry.peer_id;
      if (!peer_id.empty()) {
        relays.list.push_back(AccountStateRelay{.origin = entry.origin, .peer_id = std::move(peer_id)});
      }
    }
  }
  const std::string own = OwnAccountId();
  if (account_keys_ && !own.empty()) {
    if (auto kept = account_keys_->Kept(own); kept && *kept) {
      relays.card_issued_at = (*kept)->issued_at; // the card that lists them: the newer card's list wins a merge
    }
  }
  return relays;
}

void ConversationsHub::RestoreOurRelays(const AccountStateRelays& relays) {
  if (relays.list.empty()) {
    return; // a locker from before My relays: the deployment's default stays
  }
  std::vector<AccountCardRelay> list;
  for (const AccountStateRelay& relay : relays.list) {
    list.push_back(AccountCardRelay{.origin = relay.origin, .peer_id = relay.peer_id});
  }
  if (const auto workflow = MyRelays()) {
    workflow->Restore(list);
  }
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
    for (AccountStateGrant& grant : extras.grants) {
      restored_.grants.push_back(std::move(grant));
    }
    restored_.announce = true;
  }
  if (IsMessagingReady()) {
    ApplyRestoredCardFloors();
    AppRuntime::PostWorkerNormal([this]() {
      ResumeGroupRosterChecks();
      ApplyRestoredGrantsThenAnnounce();
    });
  }
}

void ConversationsHub::ApplyRestoredGrantsThenAnnounce() {
  // Both taken in one step: a second run (the restore's and messaging's, racing) finds neither, so the announce never
  // goes before the grants another run is still checking.
  std::vector<AccountStateGrant> restored;
  bool announce = false;
  {
    std::lock_guard lock(restored_mutex_);
    restored.swap(restored_.grants);
    announce = std::exchange(restored_.announce, false);
  }
  TakeLockerGrants(restored, true); // a restore keeps no key yet: each grantor's is looked up
  if (announce) {
    AnnounceOwnDevicesToContacts();
  }
}

void ConversationsHub::TakeLockerGrants(const std::vector<AccountStateGrant>& grants, const bool look_up_keys) {
  if (grants.empty() || !mesh_messaging_ || shutdown_requested_.load(std::memory_order_acquire)) {
    return;
  }
  std::vector<RelayGrant> taken;
  taken.reserve(grants.size());
  for (const AccountStateGrant& grant : grants) {
    taken.push_back(RelayGrant{.recipient_account_id = grant.grantor_account_id,
                               .sender_account_id = grant.grantee_account_id,
                               .issued_at = grant.issued_at,
                               .expires_at = grant.expires_at,
                               .signature = grant.signature});
  }
  mesh_messaging_->RelayGrants().TakeFromLocker(taken, look_up_keys);
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
      if (!seed || !*seed || !identity || !identity->registered) {
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
  const std::shared_ptr<IBlobClient> blob = BlobClient();
  if (!blob) {
    return Error("Blob client not configured");
  }
  auto uploaded = UploadPreparedProfileIcon(*blob, Identity(), data_dir_, prepared);
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
  const std::shared_ptr<IBlobClient> blob = BlobClient();
  if (!blob) {
    return Error("Blob client not configured");
  }
  return ClearHostedProfileIcon(*blob, Identity(), data_dir_);
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
  const std::shared_ptr<IBlobClient> blob = BlobClient();
  if (!blob) {
    return Error("Blob client not configured");
  }
  auto identity = Identity().Get();
  if (!identity) {
    return identity.error();
  }
  if (!identity->registered || identity->account_id.empty()) {
    return Error("Register on the network before using relay blob storage");
  }
  if (!IsRelayFileQuotaError(quota_error)) {
    // `/blobs` holds the profile photo alone, under its own limit: nothing there for this device to free.
    return quota_error;
  }
  // An attachment found the files relay full: free the oldest sent files until this one fits.
  return PlanOldestRelayFileDeletion(*blob, identity->account_id, AttachmentQuotaNeededBytes(quota_error));
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
  ThreadMessage target = **found;
  target.thread_id = thread_id;
  // Checked (an edit too long for one payload is refused here, before anything is stored), sent, applied here.
  auto edited = EditOwnMessage(
      Store(), target, (*thread)->kind, new_text, tapped_at_ms, util::GenerateUuid(),
      [this, &thread_id, &target](const std::string& edit_id, const std::string& text, const int64_t sent_at_ms) {
        return MeshMessaging().SendEdit(thread_id, target.id, text, edit_id, sent_at_ms);
      },
      [this](const std::string& unsent_id) { MeshMessaging().CancelPendingSend(unsent_id); });
  if (!edited) {
    return edited.error();
  }
  if (edited->unchanged) {
    return edited->row;
  }
  if (edited->stored_unsent) {
    log().warning << "edit stored but not sent (the message shows the red mark)";
  }
  Inbox().AfterMessageEdited(thread_id, target, edited->row);
  return edited->row;
}

Roe<ThreadMessage> ConversationsHub::ResendEdit(const std::string& thread_id, const std::string& message_id) {
  if (!IsInitialized()) {
    return Error("Messaging hub not initialized");
  }
  auto resent = ResendOwnEdit(
      Store(), thread_id, message_id, util::GenerateUuid(),
      [this, &thread_id, &message_id](const std::string& edit_id, const std::string& text, const int64_t sent_at_ms) {
        return MeshMessaging().SendEdit(thread_id, message_id, text, edit_id, sent_at_ms);
      },
      [this](const std::string& unsent_id) { MeshMessaging().CancelPendingSend(unsent_id); });
  if (!resent) {
    return resent.error();
  }
  if (resent->stored_unsent) {
    log().warning << "resent edit stored but not sent";
  }
  if (!resent->unsent_removed) {
    log().warning << "resent edit: the unsent edit row was not removed";
  }
  return resent->edit_row;
}

void ConversationsHub::DeleteUploadedCopy(const ThreadMessage& message) {
  const std::shared_ptr<IBlobClient> blob = BlobClient();
  if (message.content_type != ChatContentType::Attachment || !blob) {
    return;
  }
  auto fields = ChatPayloadCodec::DecodeAttachmentJson(message.payload_json);
  if (!fields || fields->url.empty()) {
    return; // sent peer-direct: nothing on the relay
  }
  auto identity = Identity().Get();
  if (!identity || identity->account_id.empty()) {
    return;
  }
  if (auto removed = DeleteUploadedAttachmentCopy(*blob, identity->account_id, fields->url); !removed) {
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
  const std::shared_ptr<IBlobClient> blob = BlobClient();
  if (!blob) {
    return Error("Blob client not configured");
  }
  auto identity = Identity().Get();
  if (!identity) {
    return identity.error();
  }
  if (!identity->registered || identity->account_id.empty()) {
    return Error("Register on the network before using relay blob storage");
  }
  return DeleteRelayFile(*blob, identity->account_id, blob_id);
}

AttachmentLimits ConversationsHub::CurrentAttachmentLimits() const {
  return DefaultAttachmentLimits();
}

void ConversationsHub::SetOnBlobSpaceWarning(std::function<void(int percent)> callback) {
  std::lock_guard lock(blob_space_mutex_);
  on_blob_space_warning_ = std::move(callback);
}

void ConversationsHub::SetOnRelayRegistrationSignal(std::function<void(RelayRegistrationSignal)> callback) {
  std::lock_guard lock(relay_registration_mu_);
  on_relay_registration_signal_ = std::move(callback);
}

void ConversationsHub::InvalidateRelayRegistrationSignals() {
  relay_registration_session_->fetch_add(1);
}

void ConversationsHub::PostRelayRegistrationSignal(const RelayRegistrationSignal signal) {
  std::function<void(RelayRegistrationSignal)> callback;
  {
    std::lock_guard lock(relay_registration_mu_); // the poll worker vs. the UI binding it
    callback = on_relay_registration_signal_;
  }
  if (!callback) {
    return;
  }
  const uint64_t session = relay_registration_session_->load();
  if (signal == RelayRegistrationSignal::PollOk) {
    AppRuntime::PostUI([callback = std::move(callback), session, current = relay_registration_session_]() {
      if (current->load() != session) {
        return; // a poll of a session that is gone
      }
      callback(RelayRegistrationSignal::PollOk);
    });
    return;
  }
  // Every poll (~2 s) says it again: one waiting on the UI thread is enough. PostUI keeps the order of the two.
  if (relay_not_registered_posted_->exchange(true)) {
    return;
  }
  AppRuntime::PostUI([callback = std::move(callback), posted = relay_not_registered_posted_, session,
                      current = relay_registration_session_]() {
    posted->store(false); // cleared when the task starts, stale or not
    if (current->load() != session) {
      return; // a poll of a session that is gone: the new session decides for itself
    }
    callback(RelayRegistrationSignal::NotRegistered);
  });
}

void ConversationsHub::CheckBlobSpace(const bool after_quota_error) {
  const std::shared_ptr<IBlobClient> blob = BlobClient();
  if (!blob_space_monitor_.ShouldCheck(util::NowUnixMs(), after_quota_error) || !blob) {
    return;
  }
  auto identity = Identity().Get();
  if (!identity || !identity->registered || identity->account_id.empty()) {
    return;
  }
  // Attachments live on the files relay (#465): its byte quota is the space that fills.
  auto listed = blob->FileList(identity->account_id, "");
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
  if (!BlobClient()) {
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

  // The send keeps the client it starts with, wherever the preferred relay moves meanwhile (MR006).
  auto run = [this, job, finish, blob = BlobClient(), name = attachment.filename, mime = attachment.mime,
              upload_opts = std::move(upload_opts)]() mutable {
    UploadChatAttachmentAsync(std::move(blob), Identity(), *job->plaintext, name, mime, std::move(upload_opts),
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
  auto run = [this, job, finish, blob = BlobClient(), record = std::move(*record),
              upload_opts = std::move(upload_opts)]() mutable {
    ResumeChatAttachmentAsync(std::move(blob), Identity(), record, std::move(upload_opts),
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

void ConversationsHub::WireRelayGrants() {
  if (!mesh_messaging_ || !contacts_ || !group_roster_) {
    return;
  }
  grant_peers_ = std::make_unique<RelayGrantPeers>(*contacts_, *group_roster_, [this]() {
    auto identity = identity_ ? identity_->Get() : Roe<LocalIdentity>(Error("no identity"));
    return identity ? identity->account_id : std::string();
  });
  RelayGrantCoordinator& grants = mesh_messaging_->RelayGrants();
  mesh_messaging_->SetRelayGrantStore(relay_grants_.get());
  grants.SetGrantedPeer([peers = grant_peers_.get()](const std::string& account) { return peers->Granted(account); });
  grants.SetBlockedPeer([peers = grant_peers_.get()](const std::string& account) { return peers->IsBlocked(account); });
  clients_.SetRelaySendHooks(grants.SendHooks());
  // Sends go into the recipient's relays, as its kept card lists them (multi-relay R2).
  clients_.Sender().SetRoutes(mesh_messaging_->RecipientRelayRoutes());
  system_controls_.Bind(grants.Family());
  // A grantee's card names a new device (a linked or restored one): our grant goes to it too (MR005).
  if (peer_cards_) {
    peer_cards_->SetOnDevicesAdded([this](const std::string& account) {
      AppRuntime::PostWorkerNormal([this, account]() {
        if (mesh_messaging_ && IsMessagingReady() && !shutdown_requested_.load(std::memory_order_acquire)) {
          mesh_messaging_->RelayGrants().NoteNewDevices(account);
        }
      });
    });
  }
}

void ConversationsHub::SweepRelayGrantsSoon() {
  AppRuntime::PostWorkerNormal([this]() {
    if (!mesh_messaging_ || !grant_peers_ || !IsMessagingReady() ||
        shutdown_requested_.load(std::memory_order_acquire)) {
      return;
    }
    RelayGrantCoordinator& grants = mesh_messaging_->RelayGrants();
    for (const std::string& blocked : grant_peers_->Blocked()) {
      grants.Stop(blocked); // blocking stops renewal; the grant they hold runs out (MR005)
    }
    grants.EnsureGranted(grant_peers_->All());
  });
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
                                 ? "Registration expired — use Renew registration in Settings → Profile."
                                 : "Brief API key rejected — renew registration in Settings → Profile.";
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
    UpdateOrgBackendClients(config_); // our relays to messaging too (WireOurRelays)
    if (actions_) {
      actions_->SetRegistrationClient(Registration());
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
  RecheckRelayIdentitiesSoon(); // a relay not checked yet is asked now (it may have failed while suspended)
  RetryOwnCardRecoverySoon();
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

void ConversationsHub::UnhookMessaging() {
  if (mesh_messaging_) {
    mesh_messaging_->SetOnBriefRelayHealthChanged(nullptr);
    mesh_messaging_->SetOnRelayNotRegistered(nullptr);
    InvalidateRelayRegistrationSignals();
    mesh_messaging_->SetOnMessagesChanged(nullptr);
    mesh_messaging_->SetOnDeliveryNotice(nullptr);
    mesh_messaging_->SetOnBackgroundUnread(nullptr);
    mesh_messaging_->SetGroupMembership(nullptr);
    mesh_messaging_->SetOnSiblingNotice(nullptr);
    mesh_messaging_->OwnAccountCards().SetOnDeviceJoined(nullptr);
    mesh_messaging_->OwnAccountCards().SetOwnCardLookup(nullptr);
    mesh_messaging_->OwnAccountCards().SetOnRecoveryChanged(nullptr);
    mesh_messaging_->SetOnInboxPolled(nullptr);
    mesh_messaging_->SetOnOwnCardChanged(nullptr);
  }
  if (inbox_) {
    inbox_->SetGroupMembership(nullptr);
    inbox_->SetCancelPendingSend(nullptr);
  }
}

void ConversationsHub::UnregisterDekConsumers() {
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
}

void ConversationsHub::Shutdown() {
  if (!initialized_) {
    return;
  }

  shutdown_requested_.store(true, std::memory_order_release);
  // My relays first: a change in progress ends (its ports stop here) before anything it touches goes; then the
  // other relays' registrations (a queued run, a revoke), before the identity they sign with.
  CloseMyRelays();
  CloseRelayRegistrations();
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
  UnhookMessaging();
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
  UnregisterDekConsumers();
  // Stop mesh (joins MeshPump) before dropping session façade.
  StopMesh();
  // Participants let go of messaging before it goes (the call sessions deliver through it).
  for (HubParticipant* participant : ParticipantsDown()) {
    participant->OnMessagingDropped();
  }
  // The relay client and the lane let go of the grants before they go with the orchestrator (and the sender, of the
  // cards it reads relays from).
  clients_.SetRelaySendHooks({});
  clients_.Sender().SetRoutes({});
  system_controls_.Unbind(relay_grant::kFamily);
  if (peer_cards_) {
    peer_cards_->SetOnDevicesAdded(nullptr);
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
  grant_peers_.reset();
  relay_grants_.reset();
  group_invite_gate_.reset();
  group_roster_.reset();
  invite_links_.reset();
  thread_prefs_.reset();
  if (inbox_) {
    inbox_->SetThreadMentions(nullptr);
  }
  thread_mentions_.reset();
  contact_refresh_.reset();
  contact_cards_.reset();
  contact_profile_refresh_.reset();
  UnwireCardReaders();
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
  CloseRelayPins();
  relay_.reset();
  directory_.SetBackend(nullptr);
  {
    std::lock_guard lock(clients_mu_);
    push_devices_.reset();
    client_compat_.reset();
    registration_.reset();
    blob_.reset();
  }
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

std::vector<DeviceNameEntry> ConversationsHub::OwnDeviceNames() {
  return account_locker_ ? account_locker_->DeviceNames() : std::vector<DeviceNameEntry>{};
}

Roe<void> ConversationsHub::RenameOwnDevice(const std::string& peer_id, const std::string& name) {
  if (!IsMessagingReady() || !mesh_messaging_ || !account_locker_) {
    return Error("Messaging is not ready");
  }
  auto list = mesh_messaging_->OwnAccountCards().Devices();
  if (!list) {
    return list.error();
  }
  if (auto allowed = CanRenameDevice(*list, peer_id); !allowed) {
    return allowed.error();
  }
  // An empty name (after NormalizeDeviceName) is a cancel: nothing is written, nothing is said.
  if (account_locker_->SetDeviceName(peer_id, name, util::NowUnixMs())) {
    PublishAccountLockerSoon();
  }
  return {};
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
  if (account_locker_) {
    account_locker_->ForgetDeviceName(peer_id, util::NowUnixMs());
  }
  log().info << "Device " << peer_id << " taken off the account; " << card->devices.size() << " left";
  AppRuntime::PostWorkerNormal([this]() { SyncLockerThenAnnounceOwnDevices(); });
  return {};
}

Roe<void> ConversationsHub::StartNewDeviceList() {
  if (!IsMessagingReady() || !mesh_messaging_) {
    return Error("Messaging is not ready");
  }
  auto card = mesh_messaging_->OwnAccountCards().StartFreshList(util::NowUnixMs());
  if (!card) {
    return card.error();
  }
  log().info << "A new device list started on this device, its main device";
  // Siblings heard already (OwnCards::SetOnChanged); our contacts and every relay of ours take the new card too.
  AppRuntime::PostWorkerNormal([this]() { ReissueAndAnnounceCard(); });
  return {};
}

void ConversationsHub::SetOnOwnDevicesChanged(std::function<void()> callback) {
  std::lock_guard lock(own_devices_changed_mu_);
  on_own_devices_changed_ = std::move(callback);
}

void ConversationsHub::NotifyOwnDevicesChanged() {
  std::function<void()> callback;
  {
    std::lock_guard lock(own_devices_changed_mu_);
    callback = on_own_devices_changed_;
  }
  if (callback) {
    callback();
  }
}

void ConversationsHub::RetryOwnCardRecoverySoon() {
  AppRuntime::PostWorkerNormal([this]() {
    if (!shutdown_requested_.load(std::memory_order_acquire) && IsMessagingReady() && mesh_messaging_) {
      mesh_messaging_->OwnAccountCards().RetryRecovery(util::NowUnixMs());
    }
  });
}

void ConversationsHub::WireOwnCardRecovery() {
  OwnCards& own = mesh_messaging_->OwnAccountCards();
  own.SetOwnCardLookup([this](const std::string& account_id, std::function<void(OwnCardAnswers)> done) {
    AppRuntime::PostWorkerNormal([this, account_id, done = std::move(done)]() {
      done(shutdown_requested_.load(std::memory_order_acquire) ? OwnCardAnswers{} : AskOurRelaysForOwnCard(account_id));
    });
  });
  own.SetOnRecoveryChanged([this]() { NotifyOwnDevicesChanged(); });
}

OwnCardAnswers ConversationsHub::AskOurRelaysForOwnCard(const std::string& account_id) const {
  // The card each relay keeps is the newest we registered there; it is ours only if our account key signed it
  // (OwnCards checks). Our relays first (preferred first), then the org's directory (CardDirectories).
  OwnCardAnswers answers;
  const auto pins = RelayIdentityPins();
  for (const std::string& origin : CardDirectories()) {
    if (pins && pins->IdentityMismatch(origin)) {
      continue; // doesn't match its link: not used (MR008)
    }
    auto stored = FetchStoredAccountCard(origin, account_id);
    if (!stored) {
      log().info << "our card not asked of " << RelayOriginKey(origin) << ": " << stored.error().message;
      ++answers.unreached;
      continue;
    }
    ++answers.answered;
    if (*stored) {
      answers.cards.push_back(**stored);
    }
  }
  return answers;
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

bool ConversationsHub::IsMainDevice() {
  if (!mesh_messaging_) {
    return true;
  }
  auto devices = mesh_messaging_->OwnAccountCards().Devices();
  return !devices || devices->devices.empty() || devices->devices.front().peer_id == devices->self_peer_id;
}

bool ConversationsHub::RunOnHubThread(std::function<void()> task) {
  if (!AppRuntime::IsRunning() || AppRuntime::CurrentlyOnUI()) {
    task();
    return true;
  }
  enum : int { kWaiting, kTaken, kGivenUp };
  auto state = std::make_shared<std::atomic<int>>(kWaiting);
  auto done = std::make_shared<std::promise<void>>();
  std::future<void> finished = done->get_future();
  AppRuntime::PostUI([task = std::move(task), state, done]() {
    int waiting = kWaiting;
    if (state->compare_exchange_strong(waiting, kTaken)) {
      task();
    }
    done->set_value();
  });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (finished.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready) {
    if (!shutdown_requested_.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
      continue;
    }
    int waiting = kWaiting;
    if (state->compare_exchange_strong(waiting, kGivenUp)) {
      log().warning << "the hub's thread did not take the task (10 s, or the hub stops); it does not run";
      return false;
    }
    finished.wait(); // taken already: it ends on the hub's thread
    return true;
  }
  return true;
}

std::shared_ptr<RelayRegistrationsWorkflow> ConversationsHub::OtherRelayRegistrations() const {
  std::lock_guard lock(relay_registrations_mu_);
  return relay_registrations_;
}

void ConversationsHub::CloseRelayRegistrations() {
  std::shared_ptr<RelayRegistrationsWorkflow> closing;
  {
    std::lock_guard lock(relay_registrations_mu_);
    closing = std::move(relay_registrations_);
  }
  if (closing) {
    closing->Close();
  }
}

std::shared_ptr<MyRelaysWorkflow> ConversationsHub::MyRelays() const {
  std::lock_guard lock(my_relays_mu_);
  return my_relays_;
}

void ConversationsHub::CloseMyRelays() {
  std::shared_ptr<MyRelaysWorkflow> closing;
  {
    std::lock_guard lock(my_relays_mu_);
    closing = std::move(my_relays_);
  }
  if (closing) {
    closing->Close();
  }
}

std::optional<RelayRegistrationRecord> ConversationsHub::PreferredRegistrationRecord() {
  auto identity = identity_ ? identity_->Get() : Roe<LocalIdentity>(Error("no identity"));
  if (!identity) {
    return std::nullopt;
  }
  RelayRegistrationRecord record{.expires_at = identity->registration_expires_at,
                                 .card_issued_at = identity->registered_card_issued_at};
  switch (ClassifyRegistration(*identity, util::NowUnixMs(), Registration()->BaseUrl())) {
    case RegistrationStatus::Active:
    case RegistrationStatus::ExpiringSoon:
      record.state = RelayRegistrationRecord::State::Registered;
      break;
    case RegistrationStatus::Expired:
      record.state = RelayRegistrationRecord::State::Expired;
      break;
    case RegistrationStatus::Unregistered:
    case RegistrationStatus::WrongBackend:
      // Never registered, or registered at the relay preferred before (a Make preferred whose renewal failed).
      record = RelayRegistrationRecord{.state = RelayRegistrationRecord::State::NotRegistered};
      break;
  }
  return record;
}

MyRelaysWorkflow::Ports ConversationsHub::MyRelaysPorts() {
  MyRelaysWorkflow::Ports ports;
  ports.probe = [this](const std::string& base_url) -> Roe<void> {
    // The cheapest unauthenticated GET every relay answers (SERVICE_ENDPOINTS § Client compatibility discovery); a
    // public address only outside developer builds (a name that resolves inside is refused at connect time).
    auto answered = HttpClientCompatClient(base_url, !config_.developer_relays).Fetch();
    if (!answered) {
      return answered.error();
    }
    return {};
  };
  ports.main_device = [this]() { return IsMainDevice(); };
  ports.apply = [this]() {
    auto applied = std::make_shared<bool>(false);
    const bool ran = RunOnHubThread([this, applied]() {
      if (!initialized_ || shutdown_requested_.load(std::memory_order_acquire)) {
        return;
      }
      UpdateOrgBackendClients(config_); // clients, inboxes and the card's relays follow the list
      if (actions_) {
        actions_->SetRegistrationClient(Registration());
      }
      *applied = true;
    });
    return ran && *applied;
  };
  ports.leave = [this](const std::string& origin) -> Roe<void> {
    if (shutdown_requested_.load(std::memory_order_acquire)) {
      return Error(MyRelaysWorkflow::kClosed);
    }
    if (auto unbound = PushDeviceCoordinator::UnregisterOn(*this, origin); !unbound) {
      log().info << "push binding on " << origin << " not removed: " << unbound.error().message;
    }
    const auto registrations = OtherRelayRegistrations();
    if (!registrations) {
      return {};
    }
    return registrations->Revoke(origin); // with the account's push devices there
  };
  ports.register_preferred = [this]() -> Roe<void> {
    if (shutdown_requested_.load(std::memory_order_acquire)) {
      return Error(MyRelaysWorkflow::kClosed);
    }
    // The card now lists the new order, so it goes whatever the auto-renew preference (ShouldPublishAccountCard),
    // and the registration records the relay it was made at (#602's backend-switch path).
    auto renewed =
        MaybeAutoRenewRegistration(*Registration(), Identity(), true, RegistrationCard(), PreferredRegistrationHome());
    if (!renewed) {
      return renewed.error();
    }
    (void)PushDeviceCoordinator::Register(*this); // the new preferred relay wakes the phone too
    return {};
  };
  ports.preferred_registration = [this]() -> std::optional<RelayRegistrationRecord> {
    if (shutdown_requested_.load(std::memory_order_acquire)) {
      return std::nullopt;
    }
    return PreferredRegistrationRecord();
  };
  ports.register_others = [this]() { RegisterOnOtherRelaysSoon(); };
  ports.announce = [this]() { ReissueAndAnnounceCard(); };
  ports.changed = [this]() { NotifyOurRelaysChanged(); };
  WireMyRelaysIdentityPorts(ports);
  return ports;
}

void ConversationsHub::WireMyRelaysIdentityPorts(MyRelaysWorkflow::Ports& ports) {
  // Each takes the profile's pins as they are when it runs (none once the profile closes).
  ports.check_identity = [this](const std::string& base_url, const std::string& peer_id) {
    const auto pins = RelayIdentityPins();
    return pins ? pins->CheckForAdd(base_url, peer_id, !config_.developer_relays)
                : AddIdentityCheck{.result = AddIdentityCheck::Result::Unavailable};
  };
  ports.drop_pin = [this](const std::string& origin) {
    // After the services left it (CheckRelayIdentitiesSoon named what stays): the org's origin keeps its pin, the
    // build's or the config's anchor.
    if (const auto pins = RelayIdentityPins(); pins && !KeepsPin(origin)) {
      pins->Drop(origin);
    }
  };
  ports.rejoin = [this](const std::string& origin) {
    if (const auto registrations = OtherRelayRegistrations()) {
      registrations->Rejoin(origin);
    }
    RegisterOnOtherRelaysSoon();
  };
  ports.identity_mismatch = [this](const std::string& origin) {
    const auto pins = RelayIdentityPins();
    return pins && pins->IdentityMismatch(origin);
  };
  ports.peer_id_of = [this](const std::string& origin) -> std::string {
    const auto pins = RelayIdentityPins();
    return pins ? pins->PinnedPeerId(origin).value_or("") : std::string();
  };
  ports.org_identity_mismatch = [this]() -> std::string {
    const auto pins = RelayIdentityPins();
    std::vector<std::string> org;
    {
      std::lock_guard lock(kept_pins_mu_);
      org = org_origins_;
    }
    const std::vector<std::string> ours = OurRelayOrigins();
    for (const std::string& origin : org) {
      const bool is_ours = std::any_of(ours.begin(), ours.end(),
                                       [&origin](const std::string& relay) { return SameRelay(relay, origin); });
      if (pins && !is_ours && pins->IdentityMismatch(origin)) {
        return origin;
      }
    }
    return {};
  };
}

Roe<OurRelaysStatus> ConversationsHub::OurRelays() {
  const auto relays = MyRelays();
  if (!relays) {
    return Error(MyRelaysWorkflow::kClosed);
  }
  return relays->Status();
}

Roe<void> ConversationsHub::AddOurRelay(const std::string& typed) {
  const auto relays = MyRelays();
  if (!relays || !IsMessagingReady()) {
    return Error(MyRelaysWorkflow::kClosed);
  }
  return relays->Add(typed, config_.developer_relays);
}

Roe<void> ConversationsHub::RemoveOurRelay(const std::string& origin) {
  const auto relays = MyRelays();
  if (!relays || !IsMessagingReady()) {
    return Error(MyRelaysWorkflow::kClosed);
  }
  return relays->Remove(origin);
}

Roe<void> ConversationsHub::MakeOurRelayPreferred(const std::string& origin) {
  const auto relays = MyRelays();
  if (!relays || !IsMessagingReady()) {
    return Error(MyRelaysWorkflow::kClosed);
  }
  return relays->MakePreferred(origin);
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
  // Our preferred relay (the one we register on) and the PeerId it answers to (MR008), so a contact adding us can
  // write before it has our card (R4); an `http://` lab relay, or one not pinned, is left out, as our card leaves it.
  const std::string preferred = NormalizeRegistrationBackend(Registration()->BaseUrl());
  if (const auto pins = RelayIdentityPins()) {
    locator.relay = AccountCardRelay{.origin = preferred, .peer_id = pins->PinnedPeerId(preferred).value_or("")};
  }
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

// Registration() never hands out null, but a build with no backend address creates no client (the UI demo app, or a
// custom backend left blank). This stands in so a call reports an error instead of dereferencing null.
class UnconfiguredRegistrationClient final : public IRegistrationClient {
public:
  IRelayAudience* Audience() const override { return nullptr; }
  Roe<RegistrationStartResult> StartRegistration(const std::string&, const std::string&, const std::string&,
                                                 const std::string&, const std::string&,
                                                 const std::vector<std::string>&,
                                                 const RegistrationPublishOpts&) override {
    return Unavailable();
  }
  Roe<RegistrationResult> FinishRegistration(const std::string&, const std::string&, const std::string&,
                                             const SignedRelayCommand&, const std::string&, const std::string&,
                                             const std::string&, const std::string&, const std::vector<std::string>&,
                                             const RegistrationPublishOpts&) override {
    return Unavailable();
  }
  Roe<RegistrationResult> UpdateNickname(const std::string&, const SignedRelayCommand&, const std::string&) override {
    return Unavailable();
  }
  Roe<void> RevokeRegistration(const std::string&, const std::string&, const SignedRelayCommand&) override {
    return Unavailable();
  }

private:
  static Error Unavailable() { return Error("registration not configured"); }
};

} // namespace

std::shared_ptr<IRegistrationClient> ConversationsHub::Registration() const {
  static const auto unconfigured = std::make_shared<UnconfiguredRegistrationClient>();
  std::lock_guard lock(clients_mu_);
  return registration_ ? registration_ : unconfigured;
}

std::string ConversationsHub::OwnAccountId() const {
  auto identity = identity_ ? identity_->Get() : Roe<LocalIdentity>(Error("no identity"));
  return identity ? identity->account_id : std::string();
}

RegistrationHome ConversationsHub::PreferredRegistrationHome() const {
  return [this]() { return Registration()->BaseUrl(); };
}

RegistrationPublishOpts ConversationsHub::RegistrationCard() {
  if (!mesh_messaging_) {
    return {};
  }
  return WithAccountCard({}, mesh_messaging_->OwnAccountCards().Card(util::NowUnixMs()));
}

std::shared_ptr<IBlobClient> ConversationsHub::BlobClient() const {
  std::lock_guard lock(clients_mu_);
  return blob_;
}

std::shared_ptr<IPushDeviceClient> ConversationsHub::PushDevices() const {
  std::lock_guard lock(clients_mu_);
  return push_devices_;
}

std::shared_ptr<IClientCompatClient> ConversationsHub::ClientCompat() const {
  std::lock_guard lock(clients_mu_);
  return client_compat_;
}

} // namespace pbr
