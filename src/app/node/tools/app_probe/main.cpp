// pp-app-probe: the app with no GUI. The same Bootstrap, ProductStack, facade and CallUiBackend the
// desktop app runs — messaging over a real relay, the control lane, AutoKey sessions, the mesh —
// driven by one scripted user through line commands on stdin. A lab driver (scripts/test/
// pp_app_lab.py) runs one probe per user and checks the outcomes.
//
// Protocol: one JSON object per line on stdin, {"id":N,"cmd":"..."}; every reply and event is one
// line on stdout prefixed "@@ " ({"id":N,"ok":true,...} / {"event":"..."}). Logs go to --log-file.
//
//   whoami                              account, nickname, registered, messaging/mesh ready
//   register {nickname}                 register on the relay (first run)
//   open_chat {account}                 find the person in the directory and open the chat
//   send {thread, text}                 a text message
//   threads / messages {thread}         what this user sees
//   call {thread, video, [accounts]}    the call button: a 1:1 chat, or a group (every other member,
//                                       or the picked `accounts`)
//   invite_to_call {account}            add someone to the call in progress
//   accept / accept_voice / decline     the ring's buttons
//   leave                               the call panel's End
//   create_group {title, accounts}      a group with these contacts (each invited)
//   group_invites                       group invites in this user's chats {nonce, group_id, title, from}
//   group_roster {group_id}             the members this user knows of
//   accept_group_invite {nonce, group_id}  the invite card's Accept → the group's thread
//   state                               the call surface + media counters (joined participants,
//                                       per-stream rx frames)
//   quit                                quit the app (orderly shutdown)
//   crash                               the process dies now (force-quit, OOM kill)
//
// Events: ready, mesh_ready, surface {kind, call_id, phase, media_status} on every change.

#include "app/Bootstrap.h"
#include "common/Logger.h"
#include "common/directory/DirectoryJson.h"
#include "domain/calls/CallLifecycleTypes.h"
#include "domain/media/CallMediaEngine.h"
#include "domain/messaging/GroupMembershipCodec.h"
#include "feature/conversations/ContactActionDispatcher.h"
#include "feature/conversations/GroupMembershipWorkflow.h"
#include "feature/calls/CallStack.h"
#include "feature/calls/CallUiBackend.h"
#include "feature/conversations/ConversationsFacade.h"
#include "feature/conversations/ConversationsHub.h"
#include "feature/product/ProductStack.h"
#include "foundation/crypto/ProfileSecretsEngine.h"
#include "foundation/data/SessionStore.h"
#include "foundation/diagnostics/LogFile.h"
#include "foundation/platform/Platform.h"
#include "foundation/runtime/AppRuntime.h"

#include <SDL3/SDL_hints.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {
namespace {

std::mutex g_out_mutex;

void Emit(const Object& line) {
  const std::string text = "@@ " + DumpJson(line) + "\n";
  std::lock_guard lock(g_out_mutex);
  std::fwrite(text.data(), 1, text.size(), stdout);
  std::fflush(stdout);
}

Object Reply(int64_t id, bool ok) {
  Object out;
  out.set("id", id);
  out.set("ok", ok);
  return out;
}

void ReplyError(int64_t id, const std::string& message) {
  Object out = Reply(id, false);
  out.set("error", message);
  Emit(out);
}

const char* SurfaceKindName(CallUiBackend::Surface::Kind kind) {
  switch (kind) {
  case CallUiBackend::Surface::Kind::None:
    return "none";
  case CallUiBackend::Surface::Kind::Ringing:
    return "ringing";
  case CallUiBackend::Surface::Kind::Answering:
    return "answering";
  case CallUiBackend::Surface::Kind::InCall:
    return "in_call";
  }
  return "?";
}

/** One scripted user: the app's objects, and what the GUI's CallController would remember. */
class Probe {
public:
  Probe() {
    product_ = std::make_unique<ProductStack>();
    product_->Hub().BindSessionStore(store_);
    product_->Hub().BindSecrets(secrets_);
    facade_ = std::make_unique<ConversationsFacade>(product_->Hub());
    product_->BindFacade(*facade_);
    calls_ = std::make_unique<CallUiBackend>(product_->Calls().Stack());
  }

  ConversationsHub& Hub() { return product_->Hub(); }
  ProfileSecretsEngine& Secrets() { return secrets_; }
  SessionStore& Store() { return store_; }

  void Wire() {
    Hub().SetOnMessagingReady([this]() {
      // The stack is (re)built: bind as ChatController::WireMessagingBindings does.
      facade_->SetOnMessagesChanged([this]() { AppRuntime::PostUI([this]() { Settle(); }); });
      Object event;
      event.set("event", "ready");
      Emit(event);
      Settle();
    });
    Hub().SetOnMeshReady([this]() {
      Object event;
      event.set("event", "mesh_ready");
      event.set("ready", facade_->IsMeshReady());
      Emit(event);
    });
    // The GUI refreshes the call surface on these (CallController::BindToMessaging, OnCallWake,
    // ChatController's messages-changed): so does the probe.
    Hub().SetOnCallWake([this]() { AppRuntime::PostUI([this]() { Settle(); }); });
  }

  /** CallController::BindToMessaging: (re)bind ring callbacks when the session manager changes. */
  void BindCalls() {
    if (!calls_->Available()) {
      bound_calls_ = nullptr;
      return;
    }
    const void* identity = calls_->SessionsIdentity();
    if (identity == bound_calls_) {
      return;
    }
    calls_->SetOnRingChanged([this]() { AppRuntime::PostUI([this]() { Settle(); }); });
    calls_->SetOnChromeRefresh([this]() { Settle(); });
    bound_calls_ = identity;
    Settle();
  }

  /** CallController::Tick: sweep every frame; refresh while a call is tracked. */
  void Tick() {
    BindCalls();
    if (!calls_->Available()) {
      return;
    }
    calls_->SweepExpiredInvites();
    if (!active_call_id_.empty()) {
      Settle();
    }
  }

  void Settle() {
    BindCalls();
    if (!calls_->Available()) {
      return;
    }
    const CallUiBackend::Surface surface = calls_->Settle();
    std::string call_id;
    switch (surface.kind) {
    case CallUiBackend::Surface::Kind::Ringing:
    case CallUiBackend::Surface::Kind::Answering:
      call_id = surface.invite->call_id;
      ringing_call_id_ = call_id;
      if (surface.active) {
        active_call_id_ = surface.active->call_id;
      }
      break;
    case CallUiBackend::Surface::Kind::InCall:
      call_id = surface.active->call_id;
      active_call_id_ = call_id;
      ringing_call_id_.clear();
      break;
    case CallUiBackend::Surface::Kind::None:
      active_call_id_.clear();
      ringing_call_id_.clear();
      break;
    }
    const std::string phase = CallPhaseName(calls_->Phase());
    const std::string status = CallMediaStatusName(calls_->MediaStatus());
    const std::string kind = SurfaceKindName(surface.kind);
    if (kind == last_kind_ && call_id == last_call_id_ && phase == last_phase_ && status == last_status_) {
      return;
    }
    last_kind_ = kind;
    last_call_id_ = call_id;
    last_phase_ = phase;
    last_status_ = status;
    Object event = SurfaceObject();
    event.set("event", "surface");
    Emit(event);
  }

  Object SurfaceObject() {
    Object out;
    out.set("kind", last_kind_);
    out.set("call_id", last_call_id_);
    out.set("phase", last_phase_);
    out.set("media_status", last_status_);
    if (calls_->Available()) {
      if (CallMediaEngine* media = product_->Calls().Stack().MediaEngine()) {
        const auto health = media->HealthSnapshot();
        out.set("media_active", media->IsActive());
        out.set("rx_audio_frames", static_cast<uint64_t>(health.rx_audio_frames));
        out.set("tx_audio_frames", static_cast<uint64_t>(health.tx_audio_frames));
        out.set("sfu_mode", health.sfu_mode);
        std::vector<Value> streams;
        for (const CallMediaStreamHealth& stream : health.streams) {
          Object row;
          row.set("stream_id", static_cast<int64_t>(stream.stream_id));
          row.set("rx_frames", static_cast<uint64_t>(stream.rx_frames));
          streams.push_back(ObjectValue(std::move(row)));
        }
        out.set("streams", ArrayValue(std::move(streams)));
      }
      std::vector<Value> joined;
      if (!last_call_id_.empty()) {
        if (auto participants = calls_->ListJoinedParticipants(last_call_id_)) {
          for (const CallParticipant& participant : *participants) {
            joined.emplace_back(participant.identity);
          }
        }
      }
      out.set("participants", ArrayValue(std::move(joined)));
      out.set("last_error", calls_->LastError());
    }
    return out;
  }

  void Handle(const std::string& line) {
    auto request = TryParseObject(line);
    if (!request) {
      ReplyError(-1, "not a JSON object");
      return;
    }
    const int64_t id = request->getIf<int64_t>("id").value_or(0);
    const std::string cmd = request->getString("cmd").value_or("");
    if (cmd == "whoami") {
      const ProfileIdentityView view = facade_->LoadProfileIdentityView();
      Object out = Reply(id, true);
      out.set("account", view.account_id);
      out.set("nickname", view.nickname);
      out.set("registered", view.registered);
      out.set("messaging_ready", Hub().IsMessagingReady());
      out.set("mesh_ready", Hub().IsMeshReady());
      Emit(out);
    } else if (cmd == "register") {
      const std::string nickname = request->getString("nickname").value_or("");
      AppRuntime::PostWorkerAndReplyOnUI<Roe<void>>(
          WorkerLane::Normal, [this, nickname]() { return facade_->RegisterIdentity(nickname); },
          [id](Roe<void> done) { done ? Emit(Reply(id, true)) : ReplyError(id, done.error().message); });
    } else if (cmd == "open_chat") {
      OpenChat(id, request->getString("account").value_or(""));
    } else if (cmd == "send") {
      const std::string thread = request->getString("thread").value_or("");
      const std::string text = request->getString("text").value_or("");
      AppRuntime::PostWorkerAndReplyOnUI<Roe<ThreadMessage>>(
          WorkerLane::Normal, [this, thread, text]() { return facade_->SendUserMessage(thread, text, {}); },
          [id](Roe<ThreadMessage> sent) {
            if (!sent) {
              ReplyError(id, sent.error().message);
              return;
            }
            Object out = Reply(id, true);
            out.set("message_id", sent->id);
            Emit(out);
          });
    } else if (cmd == "threads") {
      ListThreads(id);
    } else if (cmd == "messages") {
      ListMessages(id, request->getString("thread").value_or(""));
    } else if (cmd == "call") {
      std::vector<std::string> accounts;
      if (const Array* picked = request->getArray("accounts")) {
        for (const Value& item : picked->elements) {
          if (auto account = asString(item)) {
            accounts.push_back(*account);
          }
        }
      }
      StartCall(id, request->getString("thread").value_or(""), request->getIf<bool>("video").value_or(false),
                std::move(accounts));
    } else if (cmd == "invite_to_call") {
      InviteToCall(id, request->getString("account").value_or(""));
    } else if (cmd == "create_group") {
      std::vector<std::string> accounts;
      if (const Array* members = request->getArray("accounts")) {
        for (const Value& item : members->elements) {
          if (auto account = asString(item)) {
            accounts.push_back(*account);
          }
        }
      }
      CreateGroup(id, request->getString("title").value_or("group"), accounts);
    } else if (cmd == "group_invites") {
      ListGroupInvites(id);
    } else if (cmd == "group_roster") {
      auto roster = facade_->ListGroupRoster(request->getString("group_id").value_or(""));
      if (!roster) {
        ReplyError(id, roster.error().message);
      } else {
        std::vector<Value> members;
        for (const GroupRosterMember& member : *roster) {
          members.emplace_back(member.member_identity);
        }
        Object out = Reply(id, true);
        out.set("members", ArrayValue(std::move(members)));
        Emit(out);
      }
    } else if (cmd == "accept_group_invite") {
      AcceptGroupInvite(id, request->getString("nonce").value_or(""), request->getString("group_id").value_or(""));
    } else if (cmd == "accept" || cmd == "accept_voice") {
      Accept(id, cmd == "accept_voice");
    } else if (cmd == "decline") {
      Decline(id);
    } else if (cmd == "leave") {
      Leave(id);
    } else if (cmd == "state") {
      Settle();
      Object out = SurfaceObject();
      out.set("id", id);
      out.set("ok", true);
      Emit(out);
    } else if (cmd == "quit") {
      Emit(Reply(id, true));
      quit_ = true;
    } else if (cmd == "crash") {
      Emit(Reply(id, true));
      std::_Exit(3);
    } else {
      ReplyError(id, "unknown cmd: " + cmd);
    }
  }

  bool QuitRequested() const { return quit_; }

  /** Application::Shutdown's order, minus the GUI. */
  void Shutdown() {
    AppRuntime::BeginShutdown();
    calls_.reset();
    Hub().SetOnMessagingReady(nullptr);
    Hub().SetOnMeshReady(nullptr);
    Hub().SetOnCallWake(nullptr);
    Hub().RequestShutdown();
    if (AppRuntime::QuiesceForTeardown(std::chrono::milliseconds(3000))) {
      if (Hub().IsInitialized()) {
        Hub().Shutdown();
      }
      if (secrets_.IsInitialized()) {
        secrets_.Shutdown();
      }
    } else {
      Hub().FlushForExit();
      (void)product_.release();  // work still running may touch it
    }
    if (AppRuntime::IsRunning()) {
      AppRuntime::Shutdown();
    }
    AppRuntime::RunUITasks();
    facade_.reset();
    product_.reset();
  }

private:
  /** People search → tap the person → chat opens (ContactActionDispatcher start_conversation). */
  void OpenChat(int64_t id, const std::string& account) {
    if (account.empty()) {
      ReplyError(id, "account required");
      return;
    }
    AppRuntime::PostWorkerAndReplyOnUI<Roe<std::vector<mesh::DirectoryHit>>>(
        WorkerLane::Normal, [this, account]() { return facade_->SearchPeople(account); },
        [this, id, account](Roe<std::vector<mesh::DirectoryHit>> hits) {
          if (!hits) {
            ReplyError(id, "search: " + hits.error().message);
            return;
          }
          const mesh::DirectoryHit* found = nullptr;
          for (const mesh::DirectoryHit& hit : *hits) {
            if (hit.account_id && *hit.account_id == account) {
              found = &hit;
              break;
            }
          }
          if (!found) {
            ReplyError(id, "not in the directory: " + account);
            return;
          }
          Object action;
          action.set("type", "start_conversation");
          action.set("directory_hit", ObjectValue(DirectoryHitToJson(*found)));
          if (auto opened = Hub().Actions().Dispatch(DumpJson(action)); !opened) {
            ReplyError(id, "start_conversation: " + opened.error().message);
            return;
          }
          auto contact = facade_->FindContactByIdentity(account, mesh::ContactIdKind::Account);
          if (!contact || !*contact) {
            ReplyError(id, "contact missing after start_conversation");
            return;
          }
          auto thread = facade_->FindOrCreateDirectThread((*contact)->id, ThreadChannel::E2ePublic);
          if (!thread) {
            ReplyError(id, thread.error().message);
            return;
          }
          Object out = Reply(id, true);
          out.set("thread", thread->id);
          Emit(out);
        });
  }

  void ListThreads(int64_t id) {
    auto threads = facade_->ListThreads();
    if (!threads) {
      ReplyError(id, threads.error().message);
      return;
    }
    std::vector<Value> rows;
    for (const Thread& thread : *threads) {
      Object row;
      row.set("id", thread.id);
      row.set("title", thread.title);
      row.set("kind", thread.kind == ThreadKind::Direct ? "direct" : thread.kind == ThreadKind::Group ? "group" : "other");
      row.set("peer", thread.peer_identity_value);
      rows.push_back(ObjectValue(std::move(row)));
    }
    Object out = Reply(id, true);
    out.set("threads", ArrayValue(std::move(rows)));
    Emit(out);
  }

  void ListMessages(int64_t id, const std::string& thread) {
    auto page = facade_->GetMessagesPage(thread, std::nullopt, 200);
    if (!page) {
      ReplyError(id, page.error().message);
      return;
    }
    std::vector<Value> rows;
    for (const ThreadMessage& message : *page) {
      Object row;
      row.set("id", message.id);
      row.set("type", ChatContentTypeToDb(message.content_type));
      row.set("text", message.text);
      row.set("payload", message.payload_json);
      row.set("from", message.sender_contact_id);
      row.set("timestamp", message.timestamp);
      rows.push_back(ObjectValue(std::move(row)));
    }
    Object out = Reply(id, true);
    out.set("messages", ArrayValue(std::move(rows)));
    Emit(out);
  }

  /**
   * CallController::StartCallDirect; a group's call button opens the picker, which places the call
   * to the picked members (StartCallWithInvitees) — here every other member, or `accounts`.
   */
  void StartCall(int64_t id, const std::string& thread_id, bool video, std::vector<std::string> accounts) {
    BindCalls();
    if (!calls_->Available()) {
      ReplyError(id, "calls unavailable");
      return;
    }
    auto thread = facade_->GetThread(thread_id);
    if (!thread || !*thread) {
      ReplyError(id, "no such chat: " + thread_id);
      return;
    }
    std::vector<std::string> invitees;
    if ((*thread)->kind == ThreadKind::Group && (*thread)->group_id) {
      if (accounts.empty()) {
        auto roster = facade_->ListGroupRoster(*(*thread)->group_id);
        if (!roster) {
          ReplyError(id, roster.error().message);
          return;
        }
        const std::string self = facade_->LoadProfileIdentityView().account_id;
        for (const GroupRosterMember& member : *roster) {
          if (member.member_identity != self && !member.member_identity.empty()) {
            invitees.push_back(member.member_identity);
          }
        }
      } else {
        invitees = std::move(accounts);
      }
    } else if ((*thread)->kind == ThreadKind::Direct && !(*thread)->peer_identity_value.empty()) {
      invitees = {(*thread)->peer_identity_value};
    }
    if (invitees.empty()) {
      ReplyError(id, "nobody to call in " + thread_id);
      return;
    }
    calls_->StartCall(thread_id, video, invitees, [this, id](Roe<CallSession> started) {
      if (!started) {
        ReplyError(id, started.error().message);
        return;
      }
      active_call_id_ = started->call_id;
      Object out = Reply(id, true);
      out.set("call_id", started->call_id);
      Emit(out);
      Settle();
    });
  }

  /** CallController::InviteIdentitiesToActiveCall (the in-call "add people" picker). */
  void InviteToCall(int64_t id, const std::string& account) {
    BindCalls();
    std::string call_id = active_call_id_;
    if (call_id.empty() && calls_->Available()) {
      if (auto active = calls_->ActiveLocalCall(); active && active->has_value()) {
        call_id = (*active)->call_id;
      }
    }
    if (!calls_->Available() || call_id.empty() || account.empty()) {
      ReplyError(id, "no call in progress, or no account");
      return;
    }
    calls_->InviteParticipant(call_id, account, [id](Roe<void> invited) {
      invited ? Emit(Reply(id, true)) : ReplyError(id, invited.error().message);
    });
  }

  /** PeopleController's "New group": the picked contacts are invited to it. */
  void CreateGroup(int64_t id, const std::string& title, const std::vector<std::string>& accounts) {
    std::vector<std::string> contact_ids;
    for (const std::string& account : accounts) {
      auto contact = facade_->FindContactByIdentity(account, mesh::ContactIdKind::Account);
      if (!contact || !*contact) {
        ReplyError(id, "not a contact: " + account);
        return;
      }
      contact_ids.push_back((*contact)->id);
    }
    AppRuntime::PostWorkerAndReplyOnUI<Roe<Thread>>(
        WorkerLane::Normal, [this, title, contact_ids]() { return Hub().Groups().CreateGroup(title, contact_ids); },
        [id](Roe<Thread> thread) {
          if (!thread) {
            ReplyError(id, thread.error().message);
            return;
          }
          Object out = Reply(id, true);
          out.set("thread", thread->id);
          out.set("group_id", thread->group_id.value_or(""));
          Emit(out);
        });
  }

  /** The invite cards in this user's chats. */
  void ListGroupInvites(int64_t id) {
    auto threads = facade_->ListThreads();
    if (!threads) {
      ReplyError(id, threads.error().message);
      return;
    }
    std::vector<Value> rows;
    for (const Thread& thread : *threads) {
      if (thread.kind != ThreadKind::Direct) {
        continue;
      }
      auto page = facade_->GetMessagesPage(thread.id, std::nullopt, 200);
      if (!page) {
        continue;
      }
      for (const ThreadMessage& message : *page) {
        auto invite = GroupMembershipCodec::DecodeInviteFromMessage(message);
        if (!invite) {
          continue;
        }
        Object row;
        row.set("nonce", invite->invite_nonce);
        row.set("group_id", invite->group_id);
        row.set("title", invite->group_title);
        row.set("from", invite->inviter_identity);
        rows.push_back(ObjectValue(std::move(row)));
      }
    }
    Object out = Reply(id, true);
    out.set("invites", ArrayValue(std::move(rows)));
    Emit(out);
  }

  /** The invite card's Accept (ContactActionDispatcher accept_group_invite). */
  void AcceptGroupInvite(int64_t id, const std::string& nonce, const std::string& group_id) {
    Object action;
    action.set("type", "accept_group_invite");
    action.set("invite_nonce", nonce);
    AppRuntime::PostWorkerAndReplyOnUI<Roe<std::optional<std::string>>>(
        WorkerLane::Normal, [this, action]() { return Hub().Actions().Dispatch(DumpJson(action)); },
        [this, id, group_id](Roe<std::optional<std::string>> accepted) {
          if (!accepted) {
            ReplyError(id, accepted.error().message);
            return;
          }
          Object out = Reply(id, true);
          if (auto threads = facade_->ListThreads()) {
            for (const Thread& thread : *threads) {
              if (thread.kind == ThreadKind::Group && thread.group_id == group_id) {
                out.set("thread", thread.id);
                break;
              }
            }
          }
          Emit(out);
        });
  }

  /** CallController::AcceptIncomingImpl. */
  void Accept(int64_t id, bool voice_only) {
    BindCalls();
    std::string call_id = ringing_call_id_.empty() ? calls_->LastRingCallId() : ringing_call_id_;
    if (!calls_->Available() || call_id.empty()) {
      ReplyError(id, "nothing ringing");
      return;
    }
    calls_->SetPendingAcceptChargeDecision(InitiationChargeDecision::Waive);
    calls_->SetPendingAcceptVoiceOnly(voice_only);
    calls_->Apply(CallLifecycleEvent::AcceptClicked, call_id);
    Object out = Reply(id, true);
    out.set("call_id", call_id);
    Emit(out);
  }

  /** CallController::DeclineIncoming. */
  void Decline(int64_t id) {
    BindCalls();
    std::string call_id = ringing_call_id_;
    if (call_id.empty() && calls_->Available()) {
      call_id = calls_->LastRingCallId();
    }
    if (call_id.empty()) {
      ReplyError(id, "nothing ringing");
      return;
    }
    ringing_call_id_.clear();
    calls_->Apply(CallLifecycleEvent::DeclineClicked, call_id);
    Object out = Reply(id, true);
    out.set("call_id", call_id);
    Emit(out);
  }

  /** CallController::LeaveActive. */
  void Leave(int64_t id) {
    BindCalls();
    std::string call_id = active_call_id_;
    if (call_id.empty() && calls_->Available()) {
      call_id = calls_->ActiveCallId();
    }
    if (call_id.empty()) {
      ReplyError(id, "no call");
      return;
    }
    calls_->StopCallMedia(call_id);
    active_call_id_.clear();
    calls_->Apply(CallLifecycleEvent::LeaveClicked, call_id);
    Object out = Reply(id, true);
    out.set("call_id", call_id);
    Emit(out);
    Settle();
  }

  SessionStore store_;
  ProfileSecretsEngine secrets_;
  std::unique_ptr<ProductStack> product_;
  std::unique_ptr<ConversationsFacade> facade_;
  std::unique_ptr<CallUiBackend> calls_;
  const void* bound_calls_ = nullptr;
  std::string active_call_id_;
  std::string ringing_call_id_;
  std::string last_kind_ = "none";
  std::string last_call_id_;
  std::string last_phase_;
  std::string last_status_;
  bool quit_ = false;
};

} // namespace
} // namespace pbr

int main(int argc, char** argv) {
  std::string pin;
  std::string log_file;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--pin") == 0 && i + 1 < argc) {
      pin = argv[++i];
    } else if (std::strcmp(argv[i], "--log-file") == 0 && i + 1 < argc) {
      log_file = argv[++i];
    }
  }
  if (pin.empty()) {
    std::fprintf(stderr, "usage: pp-app-probe --config FILE --pin PIN [--log-file FILE]\n");
    return 2;
  }
  // No sound card in the lab: SDL's dummy driver plays into nothing and records silence, so the
  // real device path still runs.
  // (An SDL_AUDIO_DRIVER in the environment still wins over this hint.)
  SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");

  auto root = pbr::logging::getRootLogger();
  root.setLevel(pbr::logging::Level::INFO);
  if (!pbr::Platform::EarlyInit()) {
    std::fprintf(stderr, "platform early init failed\n");
    return 1;
  }
  if (!log_file.empty()) {
    pbr::LogFile::Install(log_file);
  }

  pbr::AppRuntimeConfig runtime;
  pbr::AppRuntime::Initialize(runtime);
  pbr::AppRuntime::InitializeUI();

  auto probe = std::make_unique<pbr::Probe>();
  probe->Wire();
  pbr::BootstrapOptions options;
  options.argc = argc;
  options.argv = argv;
  options.pin = pin;
  auto booted = pbr::Bootstrap::Run(options, probe->Hub(), probe->Secrets());
  if (!booted) {
    pbr::Object out;
    out.set("event", "boot_failed");
    out.set("error", booted.error().message);
    pbr::Emit(out);
    return 1;
  }
  probe->Store().Initialize(std::move(*booted));

  std::atomic<bool> stdin_closed{false};
  std::thread reader([&probe, &stdin_closed]() {
    std::string line;
    while (std::getline(std::cin, line)) {
      if (line.empty()) {
        continue;
      }
      pbr::AppRuntime::PostUI([&probe, line]() { probe->Handle(line); });
    }
    stdin_closed = true;
  });
  reader.detach();

  {
    pbr::Object out;
    out.set("event", "booted");
    pbr::Emit(out);
  }
  auto last_tick = std::chrono::steady_clock::now();
  while (!probe->QuitRequested() && !stdin_closed) {
    pbr::AppRuntime::RunUITasks();
    const auto now = std::chrono::steady_clock::now();
    if (now - last_tick >= std::chrono::milliseconds(50)) {  // ~ a frame at idle power-save pace
      probe->Tick();
      last_tick = now;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  probe->Shutdown();
  {
    pbr::Object out;
    out.set("event", "exited");
    pbr::Emit(out);
  }
  std::_Exit(0);  // the stdin reader may still be blocked in getline
}
