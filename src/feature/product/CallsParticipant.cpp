#include "feature/product/CallsParticipant.h"

#include "domain/calls/CallSessionLogic.h"
#include "domain/messaging/PeerThreadLogic.h"
#include "feature/calls/CallSessionManager.h"
#include "feature/calls/LocalNetworkReaction.h"
#include "feature/conversations/ConversationsHub.h"

#include <utility>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

namespace {

/** The call stack's view of the hub: its stores, delivery through messaging, the mesh objects. */
CallStackDeps MakeCallStackDeps(ConversationsHub& hub) {
  CallStackDeps deps;
  deps.address_disclosure = &hub.AddressDisclosure();
  deps.store = &hub.Store();
  deps.contacts = &hub.Contacts();
  deps.identity = &hub.Identity();
  deps.psk = hub.PskSessions();
  if (hub.MeshMessagingOrNull()) {
    deps.delivery.send_control = [&hub](const std::string& peer_identity, const ThreadMessage& control,
                                        const std::optional<std::string>& key_init_b64, const bool urgent) -> Roe<void> {
      auto* messaging = hub.MeshMessagingOrNull();
      if (!messaging) {
        return Error("Messaging not ready");
      }
      return messaging->SendControl(peer_identity, control, key_init_b64, urgent);
    };
    deps.delivery.send_to_thread = [&hub](const std::string& thread_id, const std::string& text,
                                          const SendRelayOptions& options) -> Roe<ThreadMessage> {
      auto* messaging = hub.MeshMessagingOrNull();
      if (!messaging) {
        return Error("Messaging not ready");
      }
      auto thread = hub.Store().GetThread(thread_id);
      if (!thread) {
        return thread.error();
      }
      if (*thread && (*thread)->kind == ThreadKind::Group) {
        return messaging->SendGroupMessage(thread_id, text, options);
      }
      return messaging->SendUserMessage(thread_id, text, options);
    };
    deps.delivery.resolve_peer_thread = [&hub](const std::string& peer_identity) -> Roe<std::string> {
      std::string contact_id;
      std::string title;
      if (auto contact = hub.Contacts().FindByIdentity(peer_identity, mesh::ContactIdKind::Account);
          contact && *contact) {
        contact_id = (*contact)->id;
        title = (*contact)->display_name.empty() ? (*contact)->server_nickname : (*contact)->display_name;
      }
      return ResolveDirectThreadForPeer(hub.Store(), peer_identity, contact_id, title);
    };
    deps.delivery.resolve_group_thread = [&hub](const std::string& group_id) -> std::optional<std::string> {
      auto threads = hub.Store().ListThreads();
      if (!threads) {
        return std::nullopt;
      }
      for (const Thread& thread : *threads) {
        if (thread.kind == ThreadKind::Group && thread.group_id && *thread.group_id == group_id) {
          return thread.id;
        }
      }
      return std::nullopt;
    };
    deps.delivery.group_members = [&hub](const std::string& group_id) -> std::optional<std::set<std::string>> {
      // The roster store, not Groups(): the membership workflow goes with the session; the calls may outlive it.
      GroupRosterStore* rosters = hub.GroupRosterOrNull();
      if (rosters == nullptr) {
        return std::nullopt;
      }
      auto roster = rosters->ListMembers(group_id);
      if (!roster || roster->empty()) {
        return std::nullopt;
      }
      std::set<std::string> members;
      for (const GroupRosterMember& member : *roster) {
        members.insert(member.member_identity);
      }
      return members;
    };
    deps.delivery.sync_inbox_from_wake = [&hub](bool force) {
      if (auto* messaging = hub.MeshMessagingOrNull()) {
        messaging->SyncInboxFromWake(force);
      }
    };
    deps.delivery.register_peer_direct_endpoint = [&hub](const std::string& identity, const std::string& multiaddr) {
      if (auto* messaging = hub.MeshMessagingOrNull()) {
        messaging->RegisterPeerDirectEndpoint(identity, multiaddr);
      }
    };
    deps.delivery.ensure_peer_session_key = [&hub](const std::string& peer_identity) -> Roe<EnsuredE2ePublicSessionKey> {
      auto* messaging = hub.MeshMessagingOrNull();
      if (!messaging) {
        return Error("Messaging not ready");
      }
      return messaging->EnsureE2ePublicSessionKey(peer_identity);
    };
    deps.delivery.catch_up_after_call = [&hub]() {
      if (auto* messaging = hub.MeshMessagingOrNull()) {
        messaging->SyncInboxFromWake(true);
        messaging->TailSyncActiveE2eThread();
      }
    };
    deps.system_controls = &hub.SystemControls();
  }
  const MeshConnectivityWiringInputs reach = hub.MeshInputs();
  deps.mesh = reach.mesh;
  deps.mesh_config = reach.mesh_config;
  deps.list_directory_nodes = reach.list_directory_nodes;
  deps.list_dht_nodes = reach.list_dht_nodes;
  deps.seed_dial_ok = reach.seed_dial_ok;
  deps.note_lan_mdns_peer_id = reach.note_lan_peer_id;
  deps.prefetch_peer_reachability = [&hub](const std::string& identity) { hub.PrefetchPeerReachability(identity); };
  deps.sync_mobile_ephemeral_listen = [&hub]() { hub.SyncMobileEphemeralListen(); };
  deps.connectivity = &hub.Connectivity();
  deps.media_relay = &hub.MediaRelay();
  return deps;
}

} // namespace

bool CallsParticipant::ThreadInActiveCall(const Thread& thread) {
  auto* calls = stack_.Calls();
  if (!calls) {
    return false;
  }
  auto active = calls->ActiveLocalCall();
  if (!active || !*active) {
    return false;
  }
  std::vector<CallParticipant> joined;
  if (auto parts = calls->ListJoinedParticipants((*active)->call_id); parts) {
    joined = std::move(*parts);
  }
  return ThreadMatchesActiveCall(thread, **active, joined);
}

Roe<std::vector<CallParticipant>> CallsParticipant::JoinedParticipants(const std::string& call_id) {
  auto* calls = stack_.Calls();
  if (!calls) {
    return Error("calls unavailable");
  }
  if (auto participants = calls->ListJoinedParticipants(call_id)) {
    return *participants;
  }
  return Error("call participants unavailable");
}

Roe<void> CallsParticipant::OnStoresOpened(ConversationsHub& hub) {
  return stack_.InitializeStores(hub.ProfileDbPath(), hub.ProfileId());
}

std::vector<IDekConsumer*> CallsParticipant::DekConsumers() {
  if (auto* keys = stack_.MediaKeys()) {
    return {keys};
  }
  return {};
}

void CallsParticipant::OnMessagingBuilt(ConversationsHub& hub) {
  stack_.BuildSessions(MakeCallStackDeps(hub));
  stack_.RunOnOwner([billing = hub.InitiationBilling()](CallSessionManager& calls) {
    calls.SetInitiationBillingStore(billing);
  });
}

void CallsParticipant::OnPolicyPrefs(const HubPolicyPrefs& prefs) {
  stack_.SetInboundCallAudience(prefs.call_invite_policy);
}

void CallsParticipant::OnTeardownStarting() {
  stack_.RunOnOwner([](CallSessionManager& calls) { calls.ClearMediaCallbacks(); });
}

void CallsParticipant::OnMessagingDropped() {
  stack_.ResetSessions();
}

void CallsParticipant::OnShutdown() {
  stack_.Shutdown();
}

void CallsParticipant::OnMeshMediaDetach() {
  stack_.DetachMeshMedia();
}

void CallsParticipant::OnMeshServicesStarted(ConversationsHub&) {
  stack_.OnMeshServicesStarted();
}

void CallsParticipant::OnMeshMediaRebound(ConversationsHub&) {
  stack_.RebindMeshMedia();
}

void CallsParticipant::OnMeshStopping(const std::function<void()>& abort_circuits) {
  stack_.PrepareForMeshStop(abort_circuits);
}

void CallsParticipant::OnMeshStopped() {
  stack_.FinishMeshStop();
}

void CallsParticipant::OnObservedAddressChanged() {
  stack_.OnObservedAddressChanged();
}

void CallsParticipant::OnLocalNetworkChanged(const NetworkChange& change, const mesh::LocalNetworkChange* local) {
  ReactCallsToNetworkChange(change, local, stack_);
}

void CallsParticipant::OnShutdownRequested() {
  stack_.AbortCallMediaForShutdown();
}

bool CallsParticipant::WantsEphemeralListen() const {
  return stack_.WantEphemeralListen();
}

} // namespace pbr
