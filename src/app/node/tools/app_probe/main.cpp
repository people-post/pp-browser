// pp-app-probe: the app with no GUI. The same Bootstrap, ProductStack, facade, CallUiBackend and
// CallPresenter (what the call surface shows, what its buttons do) the desktop app runs — messaging over a real relay, the control lane, AutoKey sessions, the mesh —
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
//   joinable {thread}                   the group call the call button would join {call_id} ("" none)
//   accept / accept_voice / decline     the ring's buttons
//   leave                               the call panel's End
//   show_chat {thread}                  the chat on screen (opens the thread)
//   background / foreground             a phone's OS sends the app to the background / brings it back
//   minimize / restore                  a desktop window minimized / restored
//   push_wake                           a call push arrives (what FCM's wake does)
//   stay_connected {on}                 the "Stay connected in the background" setting (P011)
//   create_group {title, accounts}      a group with these contacts (each invited)
//   group_invites                       group invites in this user's chats {nonce, group_id, title, from}
//   group_roster {group_id}             the members this user knows of
//   accept_group_invite {nonce, group_id}  the invite card's Accept → the group's thread
//   remove_member {thread, account}     the owner removes a member from the group
//   warm_seeds                          connect the bootstrap seeds now (what reconnects an idle app to
//                                       them in the field, #382)
//   backup_sessions                     what the account backup keeps of the chats: the public sessions
//   restore_sessions {bundle}           put them back as a backup restore does (each pair marked to
//                                       start its next epoch)
//   link_export                         Me → Security → Copy link payload: the link bundle {bundle}
//   link_import {bundle, pin}           a new device takes it (then restart): another device of the
//                                       account (M021)
//   state                               the call surface + media counters (joined participants,
//                                       per-stream rx frames)
//   quit                                quit the app (orderly shutdown)
//   crash                               the process dies now (force-quit, OOM kill)
//
// Events: ready, mesh_ready, surface {kind, call_id, phase, media_status} on every change.

#include "app/Bootstrap.h"
#include "common/Logger.h"
#include "common/Utilities.h"
#include "common/directory/DirectoryJson.h"
#include "lib/messaging/PeerRequestAuth.h"
#include "feature/conversations/ContactCardExchange.h"
#include "common/chat/MessagingJson.h"
#include "lib/calls/CallLifecycleTypes.h"
#include "lib/media/CallMediaEngine.h"
#include "lib/messaging/GroupMembershipCodec.h"
#include "feature/conversations/ContactActionDispatcher.h"
#include "feature/conversations/GroupMembershipWorkflow.h"
#include "feature/calls/CallPresenter.h"
#include "feature/conversations/ChatPresenter.h"
#include "feature/calls/CallStack.h"
#include "feature/calls/CallUiBackend.h"
#include "feature/conversations/ConversationsFacade.h"
#include "feature/conversations/ConversationsHub.h"
#include "feature/product/MakeCallFunctionalPorts.h"
#include "feature/product/ProductStack.h"
#include "foundation/crypto/LinkDeviceCodec.h"
#include "foundation/crypto/ProfileSecretsEngine.h"
#include "foundation/crypto/PskSessionRestart.h"
#include "lib/people/LinkDeviceCoordinator.h"
#include "foundation/data/SessionStore.h"
#include "foundation/diagnostics/LogFile.h"
#include "foundation/platform/Platform.h"
#include "foundation/runtime/AppLifecycle.h"
#include "foundation/runtime/BackgroundSyncScheduler.h"
#include "foundation/runtime/AppRuntime.h"
#include "foundation/runtime/FramePacing.h"

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

const char* NoticeKindName(CallNotice::Kind kind) {
  switch (kind) {
  case CallNotice::Kind::Error:
    return "error";
  case CallNotice::Kind::StickyError:
    return "sticky_error";
  case CallNotice::Kind::PeerEnded:
    return "peer_ended";
  case CallNotice::Kind::PeerDeclined:
    return "peer_declined";
  case CallNotice::Kind::IncomingCall:
    return "incoming_call";
  }
  return "?";
}

/** One scripted user: the app's objects; its call buttons and call screen are the GUI's (CallPresenter). */
class Probe {
public:
  Probe() {
    product_ = std::make_unique<ProductStack>();
    product_->Hub().BindSessionStore(store_);
    product_->Hub().BindSecrets(secrets_);
    facade_ = std::make_unique<ConversationsFacade>(product_->Hub());
    product_->BindFacade(*facade_);
    calls_ = std::make_unique<CallUiBackend>(product_->Calls().Stack());
    presenter_.BindPorts(MakeCallFunctionalPorts(*calls_, product_->Hub(), &store_));
    CallPresenter::Callbacks callbacks;
    callbacks.on_changed = [this]() { EmitSurface(); };
    callbacks.notify = [this](const CallNotice& notice) {
      if (notice.kind == CallNotice::Kind::Error || notice.kind == CallNotice::Kind::StickyError) {
        last_error_ = notice.key.empty() ? notice.text : notice.key;
      }
      Object event;
      event.set("event", "notice");
      event.set("kind", NoticeKindName(notice.kind));
      event.set("key", notice.key);
      event.set("text", notice.text);
      event.set("identity", notice.identity);
      Emit(event);
    };
    presenter_.BindCallbacks(std::move(callbacks));
    chats_.BindFacade(facade_.get());
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
    // The GUI refreshes the call surface on these (ChatController's messages-changed,
    // Application's call wake → CallController::OnCallWake): so does the probe.
    Hub().SetOnCallWake([this]() { presenter_.OnCallWake(); });
  }

  /** CallController::BindToMessaging. */
  void BindCalls() { presenter_.BindToMessaging(); }

  /** CallController::Tick: expired rings; the in-call screen every frame. */
  void Tick() { presenter_.Tick(); }

  /** CallController::RefreshPendingRing: the screen from the call stack now. */
  void Settle() { presenter_.Refresh(); }

  /** The screen the GUI would render: an event on every change. */
  void EmitSurface() {
    const CallScreen& screen = presenter_.Screen();
    const std::string kind = CallScreenKindName(screen.kind);
    const std::string call_id = screen.kind == CallScreen::Kind::InCall ? screen.call_id : screen.ring_call_id;
    const std::string phase = calls_->Available() ? CallPhaseName(calls_->Phase()) : "";
    const std::string status = calls_->Available() ? CallMediaStatusName(calls_->MediaStatus()) : "";
    const std::string line = screen.kind == CallScreen::Kind::InCall ? CallScreenStatusName(screen.status) : "";
    if (kind == last_kind_ && call_id == last_call_id_ && phase == last_phase_ && status == last_status_ &&
        line == last_line_) {
      return;
    }
    last_kind_ = kind;
    last_call_id_ = call_id;
    last_phase_ = phase;
    last_status_ = status;
    last_line_ = line;
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
    // What the call screen says (CallScreen): the status line and the buttons it offers.
    const CallScreen& screen = presenter_.Screen();
    out.set("status", last_line_);
    out.set("conflict", screen.conflict);
    out.set("show_retry", screen.show_retry);
    out.set("show_invite", screen.show_invite);
    out.set("group", screen.group);
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
      out.set("last_error", last_error_);
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
      if (auto identity = Hub().Identity().Get()) {
        out.set("peer_id", PeerRequestAuth::DevicePeerId(identity->public_key_b64).valueOr(""));
      }
      Emit(out);
    } else if (cmd == "locator") {
      // This account's locator, as a QR code or an invite line carries it.
      auto locator = facade_->OwnContactLocator();
      if (!locator) {
        ReplyError(id, locator.error().message);
        return;
      }
      Object out = Reply(id, true);
      out.set("locator", *locator);
      Emit(out);
    } else if (cmd == "add_from_locator") {
      // "Add contact" with a pasted invite: the contact, then the account's card from a listed device.
      facade_->AddContactFromLocator(
          request->getString("text").value_or(""), [id](Roe<ContactCardExchange::LocatorAdded> added) {
            AppRuntime::PostUI([id, added = std::move(added)]() {
              if (!added) {
                ReplyError(id, added.error().message);
                return;
              }
              Object out = Reply(id, true);
              out.set("contact", added.value().contact.id);
              out.set("keys_known", added.value().keys_known);
              Emit(out);
            });
          });
    } else if (cmd == "fetch_card") {
      // Ask device `peer` for `account`'s card over Amp (ACCOUNT_CARD.md); kept when it proves itself.
      if (!Hub().IsMessagingReady()) {
        ReplyError(id, "messaging not ready");
        return;
      }
      Hub().MeshMessaging().AccountCards().Fetch(
          request->getString("peer").value_or(""), request->getString("account").value_or(""),
          [id](Roe<PeerCardRecord> kept) {
            AppRuntime::PostUI([id, kept = std::move(kept)]() {
              if (!kept) {
                ReplyError(id, kept.error().message);
                return;
              }
              Object out = Reply(id, true);
              std::vector<Value> devices;
              for (const std::string& peer_id : kept.value().devices) {
                devices.emplace_back(peer_id);
              }
              out.set("devices", ArrayValue(std::move(devices)));
              out.set("nickname", kept.value().nickname);
              Emit(out);
            });
          });
    } else if (cmd == "kept_card") {
      // What this device keeps for `account`: where its card came from, and its devices.
      Object out = Reply(id, true);
      std::vector<Value> devices;
      if (Hub().IsMessagingReady()) {
        const std::string account = request->getString("account").value_or("");
        auto& cards = Hub().MeshMessaging().AccountCards();
        if (auto kept = cards.Kept(account); kept && *kept) {
          out.set("account_id", (*kept)->keys.account_id);
          out.set("source", (*kept)->source);
        }
        for (const std::string& peer_id : cards.DevicesOf(account)) {
          devices.emplace_back(peer_id);
        }
      }
      out.set("devices", ArrayValue(std::move(devices)));
      Emit(out);
    } else if (cmd == "register") {
      const std::string nickname = request->getString("nickname").value_or("");
      AppRuntime::PostWorkerAndReplyOnUI<Roe<void>>(
          WorkerLane::Normal, [this, nickname]() { return facade_->RegisterIdentity(nickname); },
          [id](Roe<void> done) { done ? Emit(Reply(id, true)) : ReplyError(id, done.error().message); });
    } else if (cmd == "link_export") {
      AppRuntime::PostWorkerAndReplyOnUI<Roe<std::string>>(
          WorkerLane::Normal, [this]() { return facade_->ExportLinkDevice(); },
          [id](Roe<std::string> bundle) {
            if (!bundle) {
              ReplyError(id, bundle.error().message);
              return;
            }
            Object out = Reply(id, true);
            out.set("bundle", *bundle);
            Emit(out);
          });
    } else if (cmd == "link_import") {
      const std::string bundle = request->getString("bundle").value_or("");
      const std::string pin = request->getString("pin").value_or("");
      AppRuntime::PostWorkerAndReplyOnUI<Roe<void>>(
          WorkerLane::Normal, [this, bundle, pin]() { return facade_->ImportLinkDevice(bundle, pin); },
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
    } else if (cmd == "joinable") {
      BindCalls();
      Object out = Reply(id, true);
      const auto call = calls_->Available() ? calls_->JoinableCallInThread(request->getString("thread").value_or(""))
                                            : std::nullopt;
      out.set("call_id", call.value_or(""));
      Emit(out);
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
        if (auto owner = facade_->OwnerIdentity(request->getString("group_id").value_or(""))) {
          out.set("owner", *owner);
        }
        Emit(out);
      }
    } else if (cmd == "remove_member") {
      auto thread = facade_->GetThread(request->getString("thread").value_or(""));
      if (!thread || !*thread || !(*thread)->group_id) {
        ReplyError(id, "not a group chat");
      } else if (auto removed = facade_->RemoveMemberByIdentity(*(*thread)->group_id,
                                                                request->getString("account").value_or(""));
                 !removed) {
        ReplyError(id, removed.error().message);
      } else {
        Emit(Reply(id, true));
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
    } else if (cmd == "show_chat") {
      // The chat on screen: what tapping it in the chats list does (ChatPresenter).
      chats_.SelectThread(request->getString("thread").value_or("")) ? Emit(Reply(id, true))
                                                                      : ReplyError(id, "could not open the chat");
    } else if (cmd == "pin" || cmd == "unpin" || cmd == "mute" || cmd == "unmute" || cmd == "archive" ||
               cmd == "unarchive" || cmd == "mark_unread" || cmd == "mark_read") {
      // A chat row's menu (ChatMenus::SessionActions → ChatPresenter).
      const std::string thread = request->getString("thread").value_or("");
      if (cmd == "pin" || cmd == "unpin") {
        chats_.SetPinned(thread, cmd == "pin");
      } else if (cmd == "mute") {
        chats_.Mute(thread, SessionMuteChoice::Always);
      } else if (cmd == "unmute") {
        chats_.Unmute(thread);
      } else if (cmd == "archive" || cmd == "unarchive") {
        chats_.SetArchived(thread, cmd == "archive");
      } else if (cmd == "mark_unread") {
        chats_.MarkUnread(thread);
      } else {
        chats_.MarkRead(thread);
      }
      ListThreads(id);
    } else if (cmd == "transcript") {
      Transcript(id, request->getString("thread").value_or(""));
    } else if (cmd == "share_contact") {
      // *Share contact* (ChatPresenter): contact `account` as a card into chat `thread`.
      auto contact = facade_->FindContactByIdentity(request->getString("account").value_or(""),
                                                    mesh::ContactIdKind::Account);
      if (!contact || !*contact) {
        ReplyError(id, "no such contact");
        return;
      }
      auto done = chats_.ShareContact(request->getString("thread").value_or(""), (*contact)->id);
      done ? Emit(Reply(id, true)) : ReplyError(id, done.error().message);
    } else if (cmd == "add_from_card") {
      // *Add to contacts* on a contact card in the open chat (ChatPresenter): the card checked, keys kept.
      auto added = chats_.AddContactFromCard(request->getString("message").value_or(""));
      if (!added) {
        ReplyError(id, added.error().message);
        return;
      }
      Object out = Reply(id, true);
      out.set("contact", *added);
      Emit(out);
    } else if (cmd == "chat_with") {
      // A direct chat with contact `account`, as from the contacts screen (no directory lookup).
      auto contact = facade_->FindContactByIdentity(request->getString("account").value_or(""),
                                                    mesh::ContactIdKind::Account);
      if (!contact || !*contact) {
        ReplyError(id, "no such contact");
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
    } else if (cmd == "react") {
      // A message's reaction button in the open chat (ChatPresenter): adds, or takes ours back.
      auto done = chats_.ToggleReaction(request->getString("message").value_or(""),
                                        request->getString("emoji").value_or(""));
      done ? Emit(Reply(id, true)) : ReplyError(id, done.error().message);
    } else if (cmd == "delete_for_me") {
      auto done = chats_.DeleteForMe(request->getString("message").value_or(""));
      done ? Emit(Reply(id, true)) : ReplyError(id, done.error().message);
    } else if (cmd == "delete_for_everyone") {
      const std::string message = request->getString("message").value_or("");
      if (!chats_.CanDeleteForEveryone(message)) {
        ReplyError(id, "delete for everyone is not offered for this message");
        return;
      }
      chats_.DeleteForEveryone(message, [id](Roe<void> done) {
        done ? Emit(Reply(id, true)) : ReplyError(id, done.error().message);
      });
    } else if (cmd == "quote") {
      // "Reply" on a message of the open chat: the composer quotes it (ChatPresenter).
      const std::string thread = facade_->ActiveThreadId();
      auto found = facade_->GetMessage(thread, request->getString("message").value_or(""));
      if (!found || !found->has_value()) {
        ReplyError(id, "no such message in the open chat");
        return;
      }
      chats_.StartQuoteReply((*found)->text);
      Object out = Reply(id, true);
      out.set("preview", chats_.QuotePreview());
      Emit(out);
    } else if (cmd == "compose_send") {
      // The composer's Send in the open chat: validate, add the quote, route (as ChatController).
      const std::string text = util::Trim(request->getString("text").value_or(""));
      if (auto valid = ChatPresenter::ValidateDraft(text); !valid) {
        ReplyError(id, valid.error().message);
        return;
      }
      const std::string out_text = chats_.ComposeWithQuote(text);
      chats_.CancelQuoteReply();
      auto sent = chats_.SendToChat(out_text);
      sent ? Emit(Reply(id, true)) : ReplyError(id, sent.error().message);
    } else if (cmd == "close_plan") {
      // What closing a chat would do (ChatController asks with this before it closes).
      const CloseChatPlan plan = chats_.PlanClose(request->getString("thread").value_or(""));
      Object out = Reply(id, true);
      out.set("kind", CloseChatPlanKindName(plan.kind));
      std::vector<Value> successors;
      for (const std::string& s : plan.successors) {
        successors.emplace_back(s);
      }
      out.set("successors", ArrayValue(std::move(successors)));
      Emit(out);
    } else if (cmd == "group_sheet") {
      // A group's side of the chat header's sheet (ChatPeerSheet): owner, and who the owner may remove.
      const ChatGroupSheet sheet = chats_.GroupSheetOf(request->getString("group_id").value_or(""));
      Object out = Reply(id, true);
      out.set("is_owner", sheet.is_owner);
      out.set("owner_unreachable", sheet.owner_unreachable);
      std::vector<Value> unreachable;
      for (const auto& member : sheet.unreachable_members) {
        unreachable.emplace_back(member.first);
      }
      out.set("unreachable", ArrayValue(std::move(unreachable)));
      Emit(out);
    } else if (cmd == "close_chat") {
      // Close a chat the way its plan says (an owner names a `successor`, or dismisses with none).
      const std::string thread = request->getString("thread").value_or("");
      const std::string successor = request->getString("successor").value_or("");
      const CloseChatPlan plan = chats_.PlanClose(thread);
      Roe<void> done;
      switch (plan.kind) {
      case CloseChatPlan::Kind::Delete:
      case CloseChatPlan::Kind::DeleteAi:
        done = chats_.CloseThread(thread);
        break;
      case CloseChatPlan::Kind::Dismiss:
        done = chats_.DismissGroupAndClose(thread, plan.group_id);
        break;
      case CloseChatPlan::Kind::Leave:
        done = chats_.LeaveGroupAndClose(thread, plan.group_id);
        break;
      case CloseChatPlan::Kind::LeaveAsOwner:
        done = successor.empty() ? chats_.DismissGroupAndClose(thread, plan.group_id)
                                 : chats_.HandOverAndClose(thread, plan.group_id, successor);
        break;
      }
      done ? Emit(Reply(id, true)) : ReplyError(id, done.error().message);
    } else if (cmd == "home") {
      // The Home tab: back to the chats list.
      chats_.ShowHome();
      ListThreads(id);
    } else if (cmd == "show_archived") {
      chats_.ToggleArchived();
      ListThreads(id);
    } else if (cmd == "background" || cmd == "foreground" || cmd == "minimize" || cmd == "restore") {
      // What the OS tells the app (SdlAppEvents): a phone's WILL_ENTER_BACKGROUND /
      // DID_ENTER_FOREGROUND, a desktop window minimized / restored.
      if (cmd == "background") {
        AppLifecycle::OnWillEnterBackground();
      } else if (cmd == "foreground") {
        AppLifecycle::OnDidEnterForeground();
      } else if (cmd == "minimize") {
        AppLifecycle::OnDesktopMinimized();
      } else {
        AppLifecycle::OnDesktopRestored();
      }
      Object out = Reply(id, true);
      out.set("foreground", AppLifecycle::IsForeground());
      out.set("attentive", AppLifecycle::IsUserAttentive());
      Emit(out);
    } else if (cmd == "stay_connected") {
      // Me → Profile → "Stay connected in the background" (P011): the profile pref, applied as
      // ConfigApplyBridge applies a pref change.
      ProfilePreferences prefs = store_.Snapshot().profile_prefs;
      prefs.stay_connected = request->getIf<bool>("on").value_or(true);
      if (auto saved = store_.SaveProfilePrefs(prefs); !saved) {
        ReplyError(id, saved.error().message);
      } else {
        Hub().Apply(ConversationsHub::ProjectNotifications(prefs));
        Object out = Reply(id, true);
        out.set("stay_connected", AppLifecycle::StaysConnected());
        Emit(out);
      }
    } else if (cmd == "push_wake") {
      // A call push arrived (PushWakeJni: FCM → RequestCallWakeSync).
      BackgroundSyncScheduler::Instance().RequestCallWakeSync();
      Emit(Reply(id, true));
    } else if (cmd == "warm_seeds") {
      if (!Hub().IsMeshReady()) {
        ReplyError(id, "mesh not ready");
        return;
      }
      Hub().Connectivity().Rendezvous().WarmBootstrapSeedSessions();
      Emit(Reply(id, true));
    } else if (cmd == "backup_sessions") {
      // The link bundle the local account backup seals (LocalAccountBackup::Write).
      if (!Hub().IsMessagingReady() || Secrets().Vault() == nullptr || Hub().PskStore() == nullptr) {
        ReplyError(id, "messaging not ready");
        return;
      }
      auto bundle = LinkDeviceCoordinator::ExportJson(Hub().Identity(), *Secrets().Vault(), *Hub().PskStore(),
                                                      util::NowUnixMs(), 60 * 60 * 1000);
      if (!bundle) {
        ReplyError(id, bundle.error().message);
        return;
      }
      Object out = Reply(id, true);
      out.set("bundle", *bundle);
      Emit(out);
    } else if (cmd == "restore_sessions") {
      // The sessions half of LocalAccountBackup::Restore (the account itself is still here).
      if (!Hub().IsMessagingReady() || Hub().PskStore() == nullptr) {
        ReplyError(id, "messaging not ready");
        return;
      }
      auto bundle = LinkDeviceCodec::Parse(request->getString("bundle").value_or(""));
      if (!bundle) {
        ReplyError(id, bundle.error().message);
        return;
      }
      if (auto applied = LinkDeviceCoordinator::ApplyPublicPsks(*Hub().PskStore(), bundle->public_psks); !applied) {
        ReplyError(id, applied.error().message);
        return;
      }
      if (auto marked = MarkPublicSessionsForRestart(*Hub().PskStore()); !marked) {
        ReplyError(id, marked.error().message);
        return;
      }
      Emit(Reply(id, true));
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
  /** A ring or a call panel shows (the app then runs frames faster). */
  bool CallShown() const { return presenter_.Screen().kind != CallScreen::Kind::None; }

  /** Application::Shutdown's order, minus the GUI. */
  void Shutdown() {
    AppRuntime::BeginShutdown();
    presenter_.BindCallbacks({});
    presenter_.BindPorts({});
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
          // The row's payload names the account only; the dispatcher looks it up again for the keys.
          Object action;
          action.set("type", "start_conversation");
          action.set("account_id", account);
          Hub().Actions().DispatchAsync(DumpJson(action), [this, id, account](Roe<std::optional<std::string>> opened) {
            if (!opened) {
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
        });
  }

  /**
   * Every thread, and the chats list as the user sees it (ChatPresenter: order, unread, pin, mute;
   * `listed` false for one in the other list — archived, or not while the archived list shows).
   */
  void ListThreads(int64_t id) {
    auto threads = facade_->ListThreads();
    if (!threads) {
      ReplyError(id, threads.error().message);
      return;
    }
    chats_.Refresh();
    const ChatListScreen& list = chats_.List();
    std::vector<Value> rows;
    for (const Thread& thread : *threads) {
      Object row;
      row.set("id", thread.id);
      row.set("title", thread.title);
      row.set("kind", thread.kind == ThreadKind::Direct ? "direct" : thread.kind == ThreadKind::Group ? "group" : "other");
      row.set("peer", thread.peer_identity_value);
      if (thread.group_id) {
        row.set("group_id", *thread.group_id);
      }
      row.set("listed", false);
      for (size_t i = 0; i < list.rows.size(); ++i) {
        const ChatSessionRow& shown = list.rows[i];
        if (shown.id != thread.id) {
          continue;
        }
        row.set("listed", true);
        row.set("position", static_cast<int64_t>(i));
        row.set("shown_title", shown.title);
        row.set("preview", shown.preview);
        row.set("unread", static_cast<int64_t>(shown.unread_count));
        row.set("unread_dot", shown.unread_dot);
        row.set("pinned", shown.pinned);
        row.set("muted", shown.muted);
        row.set("active", shown.active);
      }
      rows.push_back(ObjectValue(std::move(row)));
    }
    Object out = Reply(id, true);
    out.set("threads", ArrayValue(std::move(rows)));
    out.set("archived_count", static_cast<int64_t>(list.archived_count));
    out.set("showing_archived", list.showing_archived);
    Emit(out);
  }

  /** The chat's transcript as the user sees it (reactions folded onto their messages, AssembleTranscript). */
  void Transcript(int64_t id, const std::string& thread) {
    std::vector<Value> rows;
    for (const TranscriptRow& row : facade_->BuildTranscript(thread, std::nullopt)) {
      Object out;
      out.set("id", row.message.id);
      out.set("type", ChatContentTypeToDb(row.message.content_type));
      out.set("text", row.message.text);
      out.set("from", row.message.sender_contact_id);
      out.set("payload_bytes", static_cast<int64_t>(row.message.payload_json.size()));
      if (row.message.transport) {
        out.set("transport", MessageTransportToString(*row.message.transport));
      }
      std::vector<Value> reactions;
      for (const TranscriptReaction& reaction : row.reactions) {
        Object r;
        r.set("emoji", reaction.emoji);
        r.set("count", static_cast<int64_t>(reaction.count));
        r.set("mine", reaction.mine);
        reactions.push_back(ObjectValue(std::move(r)));
      }
      out.set("reactions", ArrayValue(std::move(reactions)));
      rows.push_back(ObjectValue(std::move(out)));
    }
    Object out = Reply(id, true);
    out.set("rows", ArrayValue(std::move(rows)));
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
   * A chat's call button (CallPresenter::StartCall): a 1:1 call, the group's call to join, or the
   * member picker — which here picks `accounts`, else everyone in the group but us.
   */
  void StartCall(int64_t id, const std::string& thread_id, bool video, std::vector<std::string> accounts) {
    auto finish = [id](const CallStartResult& result) {
      if (result.kind == CallStartResult::Kind::Started || result.kind == CallStartResult::Kind::Joined) {
        Object out = Reply(id, true);
        out.set("call_id", result.call_id);
        out.set("joined", result.kind == CallStartResult::Kind::Joined);
        Emit(out);
        return;
      }
      ReplyError(id, result.error_key.empty() ? result.error_text : result.error_key);
    };
    presenter_.StartCall(thread_id, video,
                         [this, thread_id, video, accounts = std::move(accounts), finish](const CallStartResult& result) {
      if (result.kind != CallStartResult::Kind::PickPeople) {
        finish(result);
        return;
      }
      presenter_.StartCallWithInvitees(thread_id, video, accounts.empty() ? OtherMembers(thread_id) : accounts,
                                       finish);
    });
  }

  /** The member picker's default: everyone in the chat's group but us. */
  std::vector<std::string> OtherMembers(const std::string& thread_id) {
    std::vector<std::string> members;
    auto thread = facade_->GetThread(thread_id);
    if (!thread || !*thread || !(*thread)->group_id) {
      return members;
    }
    auto roster = facade_->ListGroupRoster(*(*thread)->group_id);
    if (!roster) {
      return members;
    }
    const std::string self = facade_->LoadProfileIdentityView().account_id;
    for (const GroupRosterMember& member : *roster) {
      if (member.member_identity != self && !member.member_identity.empty()) {
        members.push_back(member.member_identity);
      }
    }
    return members;
  }

  /** The in-call "add people" picker's choice (CallPresenter::InviteToActiveCall). */
  void InviteToCall(int64_t id, const std::string& account) {
    if (account.empty()) {
      ReplyError(id, "account required");
      return;
    }
    presenter_.InviteToActiveCall({account}, [id](Roe<void> invited) {
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

  /** The ring's answer buttons (CallPresenter::Accept). */
  void Accept(int64_t id, bool voice_only) {
    const std::string call_id = presenter_.Accept(voice_only);
    if (call_id.empty()) {
      ReplyError(id, "nothing ringing");
      return;
    }
    Object out = Reply(id, true);
    out.set("call_id", call_id);
    Emit(out);
  }

  void Decline(int64_t id) {
    const std::string call_id = presenter_.Decline();
    if (call_id.empty()) {
      ReplyError(id, "nothing ringing");
      return;
    }
    Object out = Reply(id, true);
    out.set("call_id", call_id);
    Emit(out);
  }

  /** The call panel's End (CallPresenter::Leave). */
  void Leave(int64_t id) {
    const std::string call_id = presenter_.Leave();
    if (call_id.empty()) {
      ReplyError(id, "no call");
      return;
    }
    Object out = Reply(id, true);
    out.set("call_id", call_id);
    Emit(out);
  }

  SessionStore store_;
  ProfileSecretsEngine secrets_;
  std::unique_ptr<ProductStack> product_;
  std::unique_ptr<ConversationsFacade> facade_;
  std::unique_ptr<CallUiBackend> calls_;
  CallPresenter presenter_;
  /** The chats list, as ChatController shows it. */
  ChatPresenter chats_;
  std::string last_kind_ = "none";
  std::string last_line_;
  std::string last_error_;
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
  // Frames as the app runs them (Application's loop, BrowserHost's power save): at once when UI
  // work is posted, else after kCallChromeFrameIntervalSec while a ring / call panel shows, or
  // kIdleFrameMaxWaitSec when idle. Each frame runs the UI tasks, then ticks (CallController::Tick).
  while (!probe->QuitRequested() && !stdin_closed) {
    const double wait_sec = probe->CallShown() ? pbr::kCallChromeFrameIntervalSec : pbr::kIdleFrameMaxWaitSec;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int64_t>(wait_sec * 1000.0));
    while (!pbr::AppRuntime::HasPendingUITasks() && std::chrono::steady_clock::now() < deadline &&
           !probe->QuitRequested() && !stdin_closed) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    pbr::AppRuntime::RunUITasks();
    probe->Tick();
  }
  probe->Shutdown();
  {
    pbr::Object out;
    out.set("event", "exited");
    pbr::Emit(out);
  }
  std::_Exit(0);  // the stdin reader may still be blocked in getline
}
