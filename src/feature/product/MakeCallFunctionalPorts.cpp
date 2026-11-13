#include "feature/product/MakeCallFunctionalPorts.h"

#include "foundation/data/SessionStore.h"
#include "common/media/CallMediaHealth.h"
#include "feature/calls/CallUiBackend.h"
#include "lib/messaging/PeerThreadLogic.h"
#include "common/Localized.h"
#include "common/ValueJson.h"
#include "common/Utilities.h"
#include "common/runtime/AppRuntime.h"
#include "common/MeshAlias.h"

#include <algorithm>

namespace pbr {

std::string BlockedCallLineId(const std::string& caller) {
  return "call-blocked:" + caller;
}

Roe<std::optional<std::string>> RecordBlockedCallLine(IThreadStore& store, const std::string& thread_id,
                                                      const BlockedCallInvite& blocked, const int64_t now_ms) {
  using Words = std::optional<std::string>;
  const std::string id = BlockedCallLineId(blocked.sender_identity);
  auto existing = store.GetMessage(thread_id, id);
  if (!existing) {
    return existing.error();
  }
  int64_t count = 0;
  std::vector<std::string> call_ids;
  const int64_t call_time = blocked.expired ? blocked.created_at_ms.value_or(now_ms) : now_ms;
  int64_t line_time = call_time;
  if (existing->has_value()) {
    line_time = std::max(line_time, (*existing)->timestamp); // an old invite fetched late never moves it back
    const auto payload = TryParseObject((*existing)->payload_json);
    const auto detail = payload ? TryParseObject(payload->getString("detail").value_or("")) : std::nullopt;
    if (detail) {
      count = static_cast<int64_t>(detail->getNonNegInt("count").value_or(0));
      if (const Array* ids = detail->getArray("call_ids")) {
        for (const Value& value : ids->elements) {
          if (const auto* call_id = std::get_if<std::string>(&value)) {
            call_ids.push_back(*call_id);
          }
        }
      }
    }
  }
  if (std::find(call_ids.begin(), call_ids.end(), blocked.call_id) != call_ids.end()) {
    return Words{}; // counted already (the same invite again, e.g. redelivered after a restart)
  }
  ++count;
  call_ids.push_back(blocked.call_id);
  if (call_ids.size() > kBlockedCallIdsKept) {
    call_ids.erase(call_ids.begin(), call_ids.end() - static_cast<std::ptrdiff_t>(kBlockedCallIdsKept));
  }

  const std::string words = count == 1 ? Localized("call.blocked.row.one")
                                       : Localized("call.blocked.row.many", {{"n", std::to_string(count)}});
  // The transcript words the line from its payload, in the reader's language; the stored words stand in where no
  // line is drawn (the chat list's preview on the first write, which places the chat by the line's own time).
  std::vector<Value> ids;
  for (const std::string& call_id : call_ids) {
    ids.emplace_back(call_id);
  }
  Object detail;
  detail.set("count", count);
  detail.set("setting", mesh::InboundAudienceName(blocked.audience));
  detail.set("call_ids", makeArray(std::move(ids)));
  Object payload;
  payload.set("control_type", "call_blocked");
  payload.set("detail", DumpJson(detail));
  ThreadMessage line;
  line.id = id;
  line.thread_id = thread_id;
  line.sender_contact_id = kLocalSelfContactId;
  line.content_type = ChatContentType::System;
  line.text = words;
  line.payload_json = DumpJson(payload);
  line.timestamp = line_time;
  line.delivery = MessageDelivery::Local;
  line.relay_visible = false;
  line.generation = kLocalActionGeneration; // a local note: never part of what a model is sent
  if (auto written = store.UpsertMessage(line); !written) {
    return written.error();
  }
  if (blocked.expired) {
    return Words{}; // a call long over does not move the chat up the list to now
  }
  return Words{words};
}

CallFunctionalPorts MakeCallFunctionalPorts(CallUiBackend& backend, ConversationsHub& hub,
                                            SessionStore* session_store) {
  CallFunctionalPorts ports;
  ports.initialized = [&hub]() { return hub.IsInitialized(); };
  ports.backend = [&backend]() -> CallUiBackend* { return &backend; };
  ports.get_thread = [&hub](const std::string& thread_id) { return hub.Store().GetThread(thread_id); };
  ports.find_contact_by_identity = [&hub](const std::string& identity, const mesh::ContactIdKind kind) {
    return hub.Contacts().FindByIdentity(identity, kind);
  };
  ports.identity_icon_local_path = [&hub](const std::string& identity) { return hub.IdentityIconLocalPath(identity); };
  ports.local_account_identity = [&hub]() -> std::optional<std::string> {
    if (auto identity = hub.Identity().Get()) {
      if (!identity->account_id.empty()) {
        return identity->account_id;
      }
    }
    return std::nullopt;
  };
  ports.group_invites_in_call = [&hub](const std::string& call_id) {
    std::vector<CallGroupInvite> invites;
    for (const GroupInviteGate::InviteInCall& invite : hub.GroupInvitesInCall(call_id)) {
      invites.push_back({.invite_id = invite.invite_nonce,
                         .group_title = invite.group_title,
                         .maker_identity = invite.inviter_identity});
    }
    return invites;
  };
  ports.join_group_invite = [&hub](const std::string& invite_id, std::function<void(Roe<void>)> done) {
    // The invite card's own Accept (ContactActionDispatcher): the group's chat opens, the inviter is told.
    Object action;
    action.set("type", "accept_group_invite");
    action.set("invite_nonce", invite_id);
    hub.Actions().DispatchAsync(DumpJson(action), [done = std::move(done)](Roe<std::optional<std::string>> accepted) {
      if (done) {
        done(accepted ? Roe<void>() : Roe<void>(accepted.error()));
      }
    });
  };
  ports.invite_guest_to_group = [&hub](const std::string& group_id, const std::string& identity,
                                       const std::string& call_id, std::function<void(Roe<void>)> done) {
    // V062 4: the ordinary group invite, made in the call (it sends a DM: off the UI thread).
    AppRuntime::PostWorkerAndReplyOnUI<Roe<void>>(
        WorkerLane::Normal,
        [&hub, group_id, identity, call_id]() -> Roe<void> {
          if (AppRuntime::IsShuttingDown() || !hub.IsInitialized()) {
            return Error("Messaging not ready");
          }
          return hub.Groups().InviteFromCall(group_id, identity, call_id);
        },
        [done = std::move(done)](Roe<void> invited) {
          if (done) {
            done(std::move(invited));
          }
        });
  };
  ports.record_blocked_call = [&hub](const BlockedCallInvite& blocked) {
    if (!hub.IsInitialized()) {
      return;
    }
    // The chat its caller's messages land in, as the call's own records find it (CallsParticipant).
    std::string contact_id;
    std::string title;
    if (auto contact = hub.Contacts().FindByIdentity(blocked.sender_identity, mesh::ContactIdKind::Account);
        contact && *contact) {
      contact_id = (*contact)->id;
      title = (*contact)->display_name.empty() ? (*contact)->server_nickname : (*contact)->display_name;
    }
    auto thread = ResolveDirectThreadForPeer(hub.Store(), blocked.sender_identity, contact_id, title);
    if (!thread) {
      return;
    }
    auto words = RecordBlockedCallLine(hub.Store(), *thread, blocked, util::NowUnixMs());
    if (!words) {
      return;
    }
    // The list shows the line; no unread, no badge, no sound (not OnInboundMessagePersisted).
    if (*words) {
      (void)hub.Inbox().UpdatePreview(*thread, **words);
    }
    hub.Inbox().NotifyThreadChanged();
  };
  ports.call_diagnostics_enabled = [session_store]() {
    const bool pref =
        session_store && session_store->IsInitialized() && session_store->Snapshot().profile_prefs.call_diagnostics;
    return CallDiagnosticsEnabled(pref);
  };
  return ports;
}

} // namespace pbr
