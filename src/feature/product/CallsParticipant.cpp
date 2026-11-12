#include "feature/product/CallsParticipant.h"

#include "feature/product/CallGroupStandings.h"

#include "lib/storage/ProfileStorage.h"
#include "lib/calls/CallSessionLogic.h"
#include "lib/calls/CallSessionQueries.h"
#include "lib/messaging/PeerThreadLogic.h"
#include "feature/calls/CallSessionManager.h"
#include "feature/calls/LocalNetworkReaction.h"
#include "feature/conversations/ConversationsHub.h"

#include <utility>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

namespace {

/** The call stack's view of the hub: its stores, delivery through messaging, the mesh objects. */
CallStackDeps MakeCallStackDeps(ConversationsHub& hub, ProductMesh& mesh) {
  CallStackDeps deps;
  deps.address_disclosure = &hub.AddressDisclosure();
  deps.threads = &hub.Store();
  deps.transcript = &hub.Store();
  deps.contacts = &hub.Contacts();
  deps.account = &hub.Identity();
  deps.initiation_billing = hub.InitiationBilling();
  if (hub.MeshMessagingOrNull()) {
    deps.delivery.send_control = [&hub](const std::string& peer_identity, const ThreadMessage& control,
                                        const bool urgent) -> Roe<void> {
      auto* messaging = hub.MeshMessagingOrNull();
      if (!messaging) {
        return Error("Messaging not ready");
      }
      return messaging->SendControl(peer_identity, control, urgent);
    };
    deps.delivery.send_control_to_many = [&hub](std::vector<ControlSend> sends, ControlSendsDone done) {
      auto* messaging = hub.MeshMessagingOrNull();
      if (!messaging) {
        std::vector<ControlSendResult> results;
        for (const ControlSend& send : sends) {
          results.push_back(
              ControlSendResult{.peer_identity = send.peer_identity, .queued = Error("Messaging not ready")});
        }
        if (done) {
          done(std::move(results));
        }
        return;
      }
      messaging->SendControlToMany(std::move(sends), std::move(done));
    };
    deps.delivery.notify_own_devices = [&hub](const ThreadMessage& control) -> Roe<void> {
      auto* messaging = hub.MeshMessagingOrNull();
      if (!messaging) {
        return Error("Messaging not ready");
      }
      return messaging->NotifyOwnDevices(control);
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
      IGroupRosterStore* rosters = hub.GroupRosterOrNull();
      if (rosters == nullptr) {
        return std::nullopt;
      }
      auto roster = rosters->ListMembers(group_id);
      if (!roster) {
        return std::nullopt;
      }
      if (roster->empty()) {
        // A group the owner dissolved (G016) has no members: nobody is in it, and a call of it ends. Any other empty
        // roster is a group we do not hold, which says nothing.
        auto metadata = rosters->LoadMetadata(group_id);
        if (metadata && *metadata && (*metadata)->sync_state == GroupSyncState::Dissolved) {
          return std::set<std::string>{};
        }
        return std::nullopt;
      }
      std::set<std::string> members;
      for (const GroupRosterMember& member : *roster) {
        members.insert(member.member_identity);
      }
      return members;
    };
    // V062: who may bring and remove guests in a group's call — messaging's roster and preset, in the calls' words.
    deps.delivery.group_standing = [&hub](const std::string& group_id,
                                          const std::string& account) -> std::optional<CallGroupStanding> {
      IGroupRosterStore* rosters = hub.GroupRosterOrNull();
      return rosters ? CallStandingFromRoster(*rosters, group_id, account) : std::nullopt;
    };
    deps.delivery.sync_inbox_from_wake = [&hub](bool force) {
      if (auto* messaging = hub.MeshMessagingOrNull()) {
        messaging->SyncInboxFromWake(force);
      }
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
  deps.node_reputation = reach.node_reputation;
  deps.list_dht_nodes = reach.list_dht_nodes;
  deps.seed_dial_ok = reach.seed_dial_ok;
  deps.note_peer_device = [&hub](const std::string& account, const std::string& peer_id) {
    if (auto* messaging = hub.MeshMessagingOrNull()) {
      messaging->AccountCards().NoteDevice(account, peer_id);
    }
  };
  deps.prefetch_peer_reachability = [&hub](const std::string& identity) { hub.PrefetchPeerReachability(identity); };
  deps.sync_mobile_ephemeral_listen = [&hub]() { hub.SyncMobileEphemeralListen(); };
  deps.connectivity = &mesh.Connectivity();
  deps.media_relay = &mesh.MediaRelay();
  deps.set_on_relay_chosen = [&mesh](std::function<void(const std::string&)> hook) {
    mesh.SetOnRelayChosen(std::move(hook)); // the app's mesh keeps the slot (it remembers the relay, K015)
  };
  return deps;
}

} // namespace

bool CallsParticipant::ThreadInActiveCall(const Thread& thread) {
  const CallReads* calls = stack_.Reads();
  if (!calls) {
    return false;
  }
  auto active = calls->ActiveLocalCall();
  if (!active || !*active) {
    return false;
  }
  std::vector<CallParticipant> joined;
  if (auto parts = calls->JoinedParticipants((*active)->call_id); parts) {
    joined = std::move(*parts);
  }
  return ThreadMatchesActiveCall(thread, **active, joined);
}

bool CallsParticipant::AnyCallLive() {
  const CallReads* calls = stack_.Reads();
  if (!calls) {
    return false;
  }
  if (auto active = calls->ActiveLocalCall(); active && *active) {
    return true;
  }
  const auto invite = calls->TopPendingInvite(); // read only: no ring is started or cleared by asking
  return invite && *invite;
}

Roe<std::vector<CallParticipant>> CallsParticipant::JoinedParticipants(const std::string& call_id) {
  const CallReads* calls = stack_.Reads();
  if (!calls) {
    return Error("calls unavailable");
  }
  if (auto participants = calls->JoinedParticipants(call_id)) {
    return *participants;
  }
  return Error("call participants unavailable");
}

bool CallsParticipant::InLiveCallWith(const std::string& call_id, const std::string& identity) {
  const CallReads* calls = stack_.Reads();
  const ICallSessionStore* sessions = stack_.Sessions();
  if (!calls || !sessions) {
    return false;
  }
  auto local = calls->LocalIdentity();
  return local && call_queries::InLiveCallWith(*sessions, *local, call_id, identity);
}

Roe<void> CallsParticipant::OnStoresOpened(ConversationsHub& hub) {
  auto stores = OpenSqliteCallStores(hub.ProfileDbPath(), hub.ProfileId());
  if (!stores) {
    return stores.error();
  }
  return stack_.InitializeStores(std::move(*stores));
}

std::vector<IDekConsumer*> CallsParticipant::DekConsumers() {
  return stack_.AtRestStores();
}

void CallsParticipant::OnMessagingBuilt(ConversationsHub& hub) {
  stack_.BuildSessions(MakeCallStackDeps(hub, mesh_));
}

void CallsParticipant::OnPolicyPrefs(const HubPolicyPrefs& prefs) {
  stack_.SetInboundCallAudience(prefs.call_invite_policy);
}

void CallsParticipant::OnTeardownStarting() {
  stack_.RunOnOwner([](CallDevice& calls) { calls.DropFollowUps(); });
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

void CallsParticipant::OnGroupRosterChanged(const GroupRosterChange& change) {
  if (change.ended) {
    stack_.OnGroupMembersGone(change.group_id, std::nullopt);
  } else if (!change.member_gone.empty()) {
    stack_.OnGroupMembersGone(change.group_id, change.member_gone);
  }
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
