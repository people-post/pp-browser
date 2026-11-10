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
//   send_mentions {thread, text, mentions: [account], everyone}
//                                       a group text marking `@<nickname>` for each account (the first `@` not yet
//                                       marked, through the next space) and `@everyone` (G015)
//   threads / messages {thread}         what this user sees
//   call {thread, video, [accounts]}    the call button: a 1:1 chat, or a group (every other member,
//                                       or the picked `accounts`)
//   invite_to_call {account}            add someone to the call in progress (in a group's call someone outside the
//                                       group comes as this user's guest, V062: refused unless the preset lets them)
//   call_remove {account}               a guest's "Remove from call…" (V062: the group's owner and admins only)
//   call_add_guest_to_group {account}   a guest's "Add {name} to the group" (the ordinary invite, made in the call)
//   joinable {thread}                   the group call the call button would join {call_id} ("" none)
//   accept / accept_voice / decline     the ring's buttons
//   leave                               the call panel's End
//   show_chat {thread}                  the chat on screen (opens the thread)
//   background / foreground             a phone's OS sends the app to the background / brings it back
//   minimize / restore                  a desktop window minimized / restored
//   push_wake                           a call push arrives (what FCM's wake does)
//   stay_connected {on}                 the "Stay connected in the background" setting (P011)
//   add_to_contacts {thread}            "Add to contacts" in a direct chat with someone not in contacts
//   call_invite_policy {policy}         "Who can call me": everyone | contacts_only (a new profile's) | nobody
//   create_group {title, accounts}      a group with these contacts (each invited)
//   group_invites                       group invites in this user's chats {nonce, group_id, title, from,
//                                       member_count} (G019: the invite shows the count, never the members)
//   group_roster {group_id}             the members this user knows of {members, roles, added_by, owner, title,
//                                       preset, sync_state, roster_epoch, statement_epoch, held_changes,
//                                       changes_hash} (G018: the changes held unfolded, and the hash its group copies
//                                       carry; G019: added_by maps each member to who brought them in, "" for the
//                                       first members; G021: links, the live invite links [{link_id, maker,
//                                       expires_at, max_uses, uses}])
//   accept_group_invite {nonce, group_id}  the invite card's Accept → the group's thread
//   remove_member {thread, account}     the owner (anyone) or an admin (members, G020) removes someone from the group
//   leave_group {thread}                a member leaves the group (G018: its signed leave), the chat kept
//   set_group_role {thread, account, role} the owner makes a member admin (role admin) or member again (G017)
//   set_group_preset {thread, preset}   the owner sets the group open or managed (G017)
//   invite_to_group {thread, account}   the ordinary invite by account (G019: Open any member, Managed the owner and
//                                       admins; a member may not invite someone removed — the error says which)
//   rename_group {thread, title}        rename the group for everyone (the owner; a non-owner the preset lets: a
//                                       signed `rename` change)
//   remove_everyone_added_by {thread, account}
//                                       the owner or an admin removes everyone `account` added (G020 6) {removed}
//   reinvite_everyone_removed_by {thread, account}
//                                       the owner invites again everyone `account` removed (G020 6) {invited}
//   make_invite_link {thread, max_uses?, expires_in_s?}
//                                       group info's "Invite link…" (G021: per the preset): a new link {link, link_id,
//                                       expires_at} — `pp://join/…`, signed into the roster as a link_add
//   revoke_invite_link {thread, link_id} a link's "Revoke link" (its maker, an admin or the owner)
//   join_by_link {link}                 open / paste / scan a link: the request to its maker, waiting (up to 20 s) for
//                                       the answer {request_id, preview: {title, member_count, group_id}} or
//                                       {request_id, refused: reason} (expired / revoked / used_up / removed /
//                                       group_full / rate) or {request_id, no_answer: true} (the maker offline, or
//                                       a link it does not know / a wrong secret: those get no reply, G021)
//   confirm_join {request_id}           the preview's Join {thread, group_id}; the maker's device signs the add and
//                                       sends the join bundle
//   make_group_from_call {title?, accounts?}
//                                       the call screen's "Make a group…" (G022: a call with no group, 3+ in it) and
//                                       the picker's Create: everyone else in the call (or the picked `accounts`) is
//                                       invited, the title defaults to their names {thread, group_id, title, invited}
//   call_group_invite                   the call's group banner {shown, nonce, title, from} (G022)
//   call_group_invite_join              the banner's Join (the ordinary accept) {thread}
//   call_group_invite_not_now           the banner's Not now (the invite stays waiting)
//   warm_seeds                          connect the bootstrap seeds now (what reconnects an idle app to
//                                       them in the field, #382)
//   recovery_phrase {pin}               Me → Security → Show recovery phrase: {phrase} (24 words; "" when none)
//   locker_publish                      sync this account's state with its locker at the relay: take what another
//                                       device changed, store what changed here {version} (M023)
//   contacts                            the address book {contacts: [{account, remark}]}
//   rename_contact {account, remark}    the user's remark for a contact
//   remove_contact {account}            take the contact out of the address book
//   locker_fetch {phrase}               the locker a recovery phrase opens: {found, relay_user_id, nickname,
//                                       contacts: [account ids], groups: [group ids], card_floors}
//   locker_export {path}                save the account's state as an exported locker file at path: {version}
//   link_export                         Me → Security → Copy link payload: the link bundle {bundle}
//   devices                             this account's devices (M021 d4): {devices: [peer ids], main, self,
//                                       signed_out} — the first is the main device
//   remove_device {peer_id}             on the main device: take another device off the account
//   make_main {peer_id}                 on the main device: hand the main role to another device
//   link_import {bundle, pin}           a new device takes it (then restart): another device of the
//                                       account (M021)
//   state                               the call surface + media counters (joined participants; `members` with
//                                       guest / brought_by / can_remove / can_add_to_group, V062; ring_guest /
//                                       ring_people: ringing in as a guest and how many it shows them to;
//                                       media_epoch: the call key's epoch, which moves on each leave;
//                                       per-stream rx frames, video tx / rx and how each video stream's
//                                       frames arrived: gaps, stalls, bunched, lost; the call link's
//                                       round trip and Reliable resends)
//   quit                                quit the app (orderly shutdown)
//   crash                               the process dies now (force-quit, OOM kill)
//
// Events: ready, mesh_ready, surface {kind, call_id, phase, media_status} on every change; notice {kind, key, text,
// identity[, brought_by]} (kind guest_joined: "{identity} joined as {brought_by}'s guest", V062).
//
// --restore-phrase-file FILE: a fresh install restores the account from the recovery phrase in FILE (M023).
// --restore-saved-copy FILE: with it, an exported locker file to restore from as well as the relay's copy.
// --synthetic-video: a camera and an H.264 stand-in with real frame sizes (SyntheticVideo): video calls run in a
//   container with no camera or GPU, for measuring how video travels.

#include "lib/messaging/GroupRosterSync.h"
#include "lib/people/DevicePeerId.h"
#include "app/Bootstrap.h"
#include "common/Logger.h"
#include "common/Utilities.h"
#include "common/directory/DirectoryJson.h"
#include "lib/delivery/TextSpans.h"
#include "lib/messaging/MentionAlert.h"
#include "lib/delivery/PeerRequestAuth.h"
#include "feature/conversations/ContactCardExchange.h"
#include "common/chat/MessagingJson.h"
#include "lib/calls/CallLifecycleTypes.h"
#include "lib/media/IMediaView.h"
#include "lib/media/MediaDeviceArbiter.h"
#include "lib/media/SyntheticVideo.h"
#include "lib/messaging/GroupMembershipCodec.h"
#include "feature/conversations/ContactActionDispatcher.h"
#include "feature/conversations/GroupMembershipWorkflow.h"
#include "feature/presenters/CallPresenter.h"
#include "feature/presenters/ChatPresenter.h"
#include "feature/calls/CallStack.h"
#include "feature/calls/CallUiBackend.h"
#include "feature/conversations/ConversationsFacade.h"
#include "feature/conversations/ConversationsHub.h"
#include "feature/product/MakeCallFunctionalPorts.h"
#include "feature/product/ProductStack.h"
#include "feature/product/HubSettingsProjection.h"
#include "lib/ai/StructuredTextParser.h"
#include "lib/people/PeerDisplayLabel.h"
#include "lib/ui/PeoplePickerLogic.h"
#include "foundation/crypto/ProfileSecretsEngine.h"
#include "feature/conversations/AccountLockerWorkflow.h"
#include "lib/people/ContactClock.h"
#include "foundation/crypto/RecoveryPhrase.h"
#include "foundation/data/SessionStore.h"
#include "foundation/diagnostics/LogFile.h"
#include "foundation/platform/Platform.h"
#include "foundation/runtime/AppLifecycle.h"
#include "foundation/runtime/BackgroundSyncScheduler.h"
#include "common/runtime/AppRuntime.h"
#include "feature/product/ProductOwners.h"
#include "foundation/runtime/FramePacing.h"

#include <SDL3/SDL_hints.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <iostream>
#include <iterator>
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
    case CallNotice::Kind::PeerEnded:
      return "peer_ended";
    case CallNotice::Kind::PeerDeclined:
      return "peer_declined";
    case CallNotice::Kind::IncomingCall:
      return "incoming_call";
    case CallNotice::Kind::GuestJoined:
      return "guest_joined";
    case CallNotice::Kind::CallBack:
      return "call_back";
  }
  return "?";
}

/** One scripted user: the app's objects; its call buttons and call screen are the GUI's (CallPresenter). */
class Probe {
public:
  Probe() {
    product_ = std::make_unique<ProductStack>();
    product_->Hub().BindSecrets(secrets_);
    facade_ = std::make_unique<ConversationsFacade>(product_->Hub());
    product_->BindFacade(*facade_);
    calls_ = std::make_unique<CallUiBackend>(product_->Calls().Stack());
    presenter_.BindPorts(MakeCallFunctionalPorts(*calls_, product_->Hub(), &store_));
    CallPresenter::Callbacks callbacks;
    callbacks.on_changed = [this]() { EmitSurface(); };
    callbacks.notify = [this](const CallNotice& notice) {
      if (notice.kind == CallNotice::Kind::Error) {
        last_error_ = notice.key.empty() ? notice.text : notice.key;
      }
      Object event;
      event.set("event", "notice");
      event.set("kind", NoticeKindName(notice.kind));
      event.set("key", notice.key);
      event.set("text", notice.text);
      event.set("identity", notice.identity);
      if (!notice.brought_by.empty()) {
        event.set("brought_by", notice.brought_by); // V062: guest_joined
      }
      Emit(event);
    };
    // G022: "Make a group…" opens the picker in the app; here the command that pressed it takes the call.
    callbacks.open_make_group = [this](const std::string& call_id) { make_group_call_id_ = call_id; };
    presenter_.BindCallbacks(std::move(callbacks));
    // The preview line reads an AI reply as prose, as the app's does.
    chats_.BindCallbacks({.prose = [](const std::string& text) { return StructuredTextParser::PlainText(text); }});
    chats_.BindFacade(facade_.get());
  }

  ConversationsHub& Hub() { return product_->Hub(); }
  ProfileSecretsEngine& Secrets() { return secrets_; }
  SessionStore& Store() { return store_; }

  void Wire() {
    Hub().SetOnMessagingReady([this]() {
      // The stack is (re)built: bind as ChatController::WireMessagingBindings does.
      facade_->SetOnMessagesChanged([this]() { AppRuntime::PostUI([this]() { Settle(); }); });
      // A background message notice, decided as ChatController does (muted chats: mentions only, G015).
      facade_->SetOnBackgroundUnread([this](std::string, std::string, std::string thread_id, bool mentions_us) {
        const std::optional<ThreadLocalPrefs> prefs = chats_.PrefsOf(thread_id);
        const int64_t now_ms = util::NowUnixMs();
        const bool muted = prefs && ThreadIsMuted(prefs->muted_until, now_ms);
        Object event;
        event.set("event", "notice");
        event.set("kind", "message");
        event.set("thread", thread_id);
        event.set("mentions_us", mentions_us);
        event.set("alerts", NoticeAlerts(muted, prefs && prefs->mute_mentions, mentions_us,
                                         mention_alert_ms_[thread_id], now_ms));
        Emit(event);
      });
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

  void SendMentions(const int64_t id, const Object& request) {
    const std::string thread = request.getString("thread").value_or("");
    const std::string text = request.getString("text").value_or("");
    std::vector<TextSpan> spans;
    size_t from = 0;
    const auto mark = [&](const std::string& token, TextSpan span) {
      const size_t at = text.find(token, from);
      if (at == std::string::npos) {
        return;
      }
      size_t end = text.find(' ', at);
      end = end == std::string::npos ? text.size() : end;
      span.start = static_cast<uint32_t>(at);
      span.end = static_cast<uint32_t>(end);
      spans.push_back(std::move(span));
      from = end;
    };
    if (const Array* mentions = request.getArray("mentions")) {
      for (const Value& value : mentions->elements) {
        if (const auto account = asString(value)) {
          mark("@", TextSpan{.kind = static_cast<uint8_t>(TextSpanKind::Mention), .account_id = *account});
        }
      }
    }
    if (request.getIf<bool>("everyone").value_or(false)) {
      from = 0;
      mark("@everyone", TextSpan{.kind = static_cast<uint8_t>(TextSpanKind::Everyone)});
    }
    AppRuntime::PostWorkerAndReplyOnUI<Roe<ThreadMessage>>(
        WorkerLane::Normal,
        [this, thread, text, spans = std::move(spans)]() mutable {
          return facade_->SendGroupText(thread, text, std::move(spans));
        },
        [id](Roe<ThreadMessage> sent) {
          if (!sent) {
            ReplyError(id, sent.error().message);
            return;
          }
          Object out = Reply(id, true);
          out.set("message_id", sent->id);
          Emit(out);
        });
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

  /** V062: the call screen's people — guests, who brought them, and the guest buttons this user has. */
  static std::vector<Value> MembersOf(const CallScreen& screen) {
    std::vector<Value> members;
    for (const CallScreen::Member& member : screen.members) {
      Object row;
      row.set("identity", member.identity);
      row.set("guest", member.guest);
      row.set("brought_by", member.brought_by);
      row.set("can_remove", member.can_remove);
      row.set("can_add_to_group", member.can_add_to_group);
      members.push_back(ObjectValue(std::move(row)));
    }
    return members;
  }

  /** What the call screen's media frame decided (CallMediaFrame): the status line, the reconnect and its capsule. */
  static Object MediaFrameObject(const CallMediaScreen& frame) {
    Object out;
    out.set("line", frame.line.key);
    out.set("reconnect", frame.media.reconnect);
    out.set("capsule", std::string(CallReconnectKey(frame.reconnect.kind)));
    out.set("on_hold", frame.media.on_hold);
    out.set("starting_video", frame.media.starting_video);
    out.set("hint", frame.hint_shown ? frame.hint_key : std::string());
    return out;
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
    out.set("show_make_group", screen.show_make_group); // G022
    out.set("group_invite", screen.group_invite ? screen.group_invite->invite_id : std::string());
    out.set("group", screen.group);
    out.set("members", ArrayValue(MembersOf(screen))); // V062: who is a guest, and what this user may do
    out.set("ring_guest", screen.ring_guest);
    out.set("ring_people", static_cast<int64_t>(screen.ring_people));
    out.set("media_frame", ObjectValue(MediaFrameObject(presenter_.MediaScreen())));
    if (calls_->Available()) {
      IMediaView& media = calls_->Media();
      const auto health = media.HealthSnapshot();
      out.set("media_active", media.IsActive());
      out.set("rx_audio_frames", static_cast<uint64_t>(health.rx_audio_frames));
      out.set("tx_audio_frames", static_cast<uint64_t>(health.tx_audio_frames));
      out.set("sfu_mode", health.packet_mode);
      out.set("path", CallMediaPathName(calls_->MediaPath()));
      std::vector<Value> streams;
      for (const MediaStreamHealth& stream : health.streams) {
        Object row;
        row.set("stream_id", static_cast<int64_t>(stream.stream_id));
        row.set("rx_frames", static_cast<uint64_t>(stream.rx_frames));
        streams.push_back(ObjectValue(std::move(row)));
      }
      out.set("streams", ArrayValue(std::move(streams)));
      out.set("tx_video_frames", static_cast<uint64_t>(health.tx_video_frames));
      out.set("rx_video_frames", static_cast<uint64_t>(health.rx_video_frames));
      std::vector<Value> video;
      for (const VideoStreamArrival& stream : health.video_arrivals) {
        const VideoArrivalSummary& a = stream.arrival;
        Object row;
        row.set("stream_id", static_cast<int64_t>(stream.stream_id));
        row.set("frames", static_cast<uint64_t>(a.frames));
        row.set("lost", static_cast<uint64_t>(a.lost));
        row.set("gap_p50_ms", a.gap_p50_ms);
        row.set("gap_p95_ms", a.gap_p95_ms);
        row.set("gap_max_ms", a.gap_max_ms);
        row.set("stalls", static_cast<uint64_t>(a.stalls));
        row.set("bunched", static_cast<uint64_t>(a.bunched));
        video.push_back(ObjectValue(std::move(row)));
      }
      out.set("video_arrivals", ArrayValue(std::move(video)));
      // The call's link (Amp ConnectionStats of the association its media rides).
      const CallLinkCounters link = calls_->MediaLinkCounters();
      if (link.available) {
        Object row;
        row.set("link_id", link.link_id);
        row.set("reliable_sent", link.reliable_sent);
        row.set("retransmits", link.retransmits);
        row.set("srtt_ms", link.srtt_ms);
        out.set("link", ObjectValue(std::move(row)));
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
      // The call's media key epoch: it moves on each leave (V003), a guest's removal too (V062).
      if (auto active = calls_->ActiveLocalCall(); active && active->has_value()) {
        out.set("media_epoch", static_cast<int64_t>((*active)->media_epoch));
      }
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
        out.set("peer_id", DevicePeerIdFromKey(identity->public_key_b64).valueOr(""));
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
      facade_->AddContactFromLocator(request->getString("text").value_or(""),
                                     [id](Roe<ContactCardExchange::LocatorAdded> added) {
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
      Hub().MeshMessaging().AccountCards().Fetch(request->getString("peer").value_or(""),
                                                 request->getString("account").value_or(""),
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
    } else if (cmd == "devices") {
      AppRuntime::PostWorkerAndReplyOnUI<Roe<OwnDeviceList>>(
          WorkerLane::Normal, [this]() { return facade_->OwnDevices(); },
          [id](Roe<OwnDeviceList> list) {
            if (!list) {
              ReplyError(id, list.error().message);
              return;
            }
            Object out = Reply(id, true);
            std::vector<Value> devices;
            for (const AccountCardDevice& device : list->devices) {
              devices.emplace_back(device.peer_id);
            }
            out.set("main", list->devices.empty() ? std::string() : list->devices.front().peer_id);
            out.set("devices", ArrayValue(std::move(devices)));
            out.set("self", list->self_peer_id);
            out.set("signed_out", list->signed_out);
            Emit(out);
          });
    } else if (cmd == "remove_device" || cmd == "make_main") {
      const std::string peer_id = request->getString("peer_id").value_or("");
      const bool remove = cmd == "remove_device";
      AppRuntime::PostWorkerAndReplyOnUI<Roe<void>>(
          WorkerLane::Normal,
          [this, peer_id, remove]() {
            return remove ? facade_->RemoveOwnDevice(peer_id) : facade_->MakeMainDevice(peer_id);
          },
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
    } else if (cmd == "recovery_phrase") {
      const std::string pin = request->getString("pin").value_or("");
      AppRuntime::PostWorkerAndReplyOnUI<Roe<std::optional<SecretText>>>(
          WorkerLane::Normal, [this, pin]() { return facade_->RecoveryPhrase(pin); },
          [id](Roe<std::optional<SecretText>> phrase) {
            if (!phrase) {
              ReplyError(id, phrase.error().message);
              return;
            }
            Object out = Reply(id, true);
            // The test probe hands the phrase to its driver (lab: restore on another device).
            out.set("phrase", phrase->has_value() ? (*phrase)->Reveal() : std::string());
            Emit(out);
          });
    } else if (cmd == "contacts") {
      auto listed = Hub().Contacts().List();
      if (!listed) {
        ReplyError(id, listed.error().message);
        return;
      }
      std::vector<Value> rows;
      for (const Contact& contact : *listed) {
        Object row;
        row.set("account", ContactClockAccount(contact));
        row.set("remark", contact.local.display_name);
        rows.push_back(ObjectValue(std::move(row)));
      }
      Object out = Reply(id, true);
      out.set("contacts", ArrayValue(std::move(rows)));
      Emit(out);
    } else if (cmd == "rename_contact" || cmd == "remove_contact") {
      auto found =
          Hub().Contacts().FindByIdentity(request->getString("account").value_or(""), mesh::ContactIdKind::Account);
      if (!found || !*found) {
        ReplyError(id, "no such contact");
        return;
      }
      if (cmd == "remove_contact") {
        auto removed = Hub().Contacts().Remove((*found)->id);
        removed ? Emit(Reply(id, true)) : ReplyError(id, removed.error().message);
        return;
      }
      Contact contact = **found;
      contact.local.display_name = request->getString("remark").value_or("");
      SyncContactMirrors(contact);
      auto saved = Hub().Contacts().Upsert(contact);
      saved ? Emit(Reply(id, true)) : ReplyError(id, saved.error().message);
    } else if (cmd == "locker_publish") {
      AppRuntime::PostWorkerAndReplyOnUI<Roe<uint64_t>>(
          WorkerLane::Normal,
          [this]() {
            AccountLockerWorkflow* locker = Hub().AccountLocker();
            return locker ? locker->Publish() : Roe<uint64_t>(Error("messaging not ready"));
          },
          [id](Roe<uint64_t> version) {
            if (!version) {
              ReplyError(id, version.error().message);
              return;
            }
            Object out = Reply(id, true);
            out.setJsonUInt("version", *version);
            Emit(out);
          });
    } else if (cmd == "locker_export") {
      const std::string path = request->getString("path").value_or("");
      AppRuntime::PostWorkerAndReplyOnUI<Roe<std::string>>(
          WorkerLane::Normal, [this]() { return facade_->ExportAccountCopy(); },
          [id, path](Roe<std::string> file) {
            if (!file) {
              ReplyError(id, file.error().message);
              return;
            }
            auto record = LockerRecordFromFileJson(*file);
            std::ofstream out_file(path, std::ios::binary | std::ios::trunc);
            out_file << *file;
            if (!record || !out_file) {
              ReplyError(id, "the saved copy was not written");
              return;
            }
            Object out = Reply(id, true);
            out.setJsonUInt("version", record->version);
            Emit(out);
          });
    } else if (cmd == "locker_fetch") {
      const std::string phrase = request->getString("phrase").value_or("");
      using Fetched = std::optional<AccountLockerWorkflow::Fetched>;
      AppRuntime::PostWorkerAndReplyOnUI<Roe<Fetched>>(
          WorkerLane::Normal,
          [this, phrase]() -> Roe<Fetched> {
            auto seed = DecodeRecoveryPhrase(phrase);
            if (!seed) {
              return seed.error();
            }
            AccountLockerWorkflow* locker = Hub().AccountLocker();
            return locker ? locker->Fetch(*seed) : Roe<Fetched>(Error("messaging not ready"));
          },
          [id](Roe<Fetched> state) {
            if (!state) {
              ReplyError(id, state.error().message);
              return;
            }
            Object out = Reply(id, true);
            out.set("found", state->has_value());
            if (*state) {
              out.setJsonUInt("version", (*state)->version);
              const AccountLockerState& s = (*state)->state;
              out.set("relay_user_id", s.relay_user_id);
              out.set("nickname", s.nickname);
              std::vector<Value> contacts;
              for (const Contact& contact : s.contacts) {
                for (const mesh::ContactId& cid : contact.remote.ids) {
                  if (cid.kind == mesh::ContactIdKind::Account) {
                    contacts.emplace_back(cid.value);
                  }
                }
              }
              out.set("contacts", ArrayValue(std::move(contacts)));
              std::vector<Value> groups;
              for (const AccountStateGroup& group : s.extras.groups) {
                groups.emplace_back(group.group_id);
              }
              out.set("groups", ArrayValue(std::move(groups)));
              out.set("card_floors", static_cast<int64_t>(s.extras.card_floors.size()));
            }
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
      const auto call =
          calls_->Available() ? calls_->JoinableCallInThread(request->getString("thread").value_or("")) : std::nullopt;
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
        Object roles;
        Object added_by;
        for (const GroupRosterMember& member : *roster) {
          members.emplace_back(member.member_identity);
          roles.set(member.member_identity, MemberRoleToString(member.role));
          added_by.set(member.member_identity, member.added_by);
        }
        Object out = Reply(id, true);
        out.set("members", ArrayValue(std::move(members)));
        out.set("roles", std::move(roles));       // G017: owner / admin / member, as the signed roster says
        out.set("added_by", std::move(added_by)); // G019: who brought each in ("" for the first members)
        if (auto owner = facade_->OwnerIdentity(request->getString("group_id").value_or(""))) {
          out.set("owner", *owner);
        }
        // G013: a restored group's member check (active / checking / removed) and the roster epoch.
        if (IGroupRosterStore* rosters = Hub().GroupRosterOrNull()) {
          if (auto metadata = rosters->LoadMetadata(request->getString("group_id").value_or(""));
              metadata && *metadata) {
            out.set("sync_state", GroupSyncStateToString((*metadata)->sync_state));
            out.setJsonUInt("roster_epoch", (*metadata)->roster_epoch);
            out.set("statement", !(*metadata)->roster_statement_json.empty());
            out.set("preset", GroupPresetToString((*metadata)->policy.preset));
            out.set("title", (*metadata)->title);
          }
          // G018: the changes held on top of the owner's statement, and what this device's copies say of them.
          const std::string group_id = request->getString("group_id").value_or("");
          const GroupCopyState state = group_roster_sync::CopyState(*rosters, group_id);
          out.setJsonUInt("statement_epoch", state.statement_epoch);
          out.set("changes_hash", state.changes_hash);
          auto held = rosters->ListGroupChanges(group_id);
          out.setJsonUInt("held_changes", held ? static_cast<uint64_t>(held->size()) : 0);
        }
        out.set("links", LinkRows(request->getString("group_id").value_or("")));
        Emit(out);
      }
    } else if (cmd == "remove_member") {
      auto thread = facade_->GetThread(request->getString("thread").value_or(""));
      if (!thread || !*thread || !(*thread)->group_id) {
        ReplyError(id, "not a group chat");
      } else if (auto removed =
                     facade_->RemoveMemberByIdentity(*(*thread)->group_id, request->getString("account").value_or(""));
                 !removed) {
        ReplyError(id, removed.error().message);
      } else {
        Emit(Reply(id, true));
      }
    } else if (cmd == "leave_group") {
      auto thread = facade_->GetThread(request->getString("thread").value_or(""));
      if (!thread || !*thread || !(*thread)->group_id) {
        ReplyError(id, "not a group chat");
      } else if (auto left = facade_->LeaveGroup(*(*thread)->group_id); !left) {
        ReplyError(id, left.error().message);
      } else {
        Emit(Reply(id, true));
      }
    } else if (cmd == "set_group_role" || cmd == "set_group_preset") {
      auto thread = facade_->GetThread(request->getString("thread").value_or(""));
      Roe<void> set = Error("not a group chat");
      if (thread && *thread && (*thread)->group_id) {
        const std::string group_id = *(*thread)->group_id;
        if (cmd == "set_group_role") {
          const std::string role = request->getString("role").value_or("");
          set = role == "admin" || role == "member"
                    ? facade_->SetMemberRole(group_id, request->getString("account").value_or(""),
                                             MemberRoleFromString(role))
                    : Roe<void>(Error("role must be admin or member"));
        } else {
          const std::string preset = request->getString("preset").value_or("");
          set = preset == "open" || preset == "managed"
                    ? facade_->SetGroupPreset(group_id, GroupPresetFromString(preset))
                    : Roe<void>(Error("preset must be open or managed"));
        }
      }
      if (!set) {
        ReplyError(id, set.error().message);
      } else {
        Emit(Reply(id, true));
      }
    } else if (cmd == "invite_to_group" || cmd == "rename_group" || cmd == "remove_everyone_added_by" ||
               cmd == "reinvite_everyone_removed_by") {
      ChangeGroup(id, cmd, request->getString("thread").value_or(""), request->getString("account").value_or(""),
                  request->getString("title").value_or(""));
    } else if (cmd == "make_invite_link" || cmd == "revoke_invite_link") {
      InviteLink(id, cmd, *request);
    } else if (cmd == "join_by_link") {
      JoinByLink(id, request->getString("link").value_or(""));
    } else if (cmd == "confirm_join") {
      ConfirmJoin(id, request->getString("request_id").value_or(""));
    } else if (cmd == "make_group_from_call") {
      std::vector<std::string> accounts;
      if (const Array* picked = request->getArray("accounts")) {
        for (const Value& item : picked->elements) {
          if (auto account = asString(item)) {
            accounts.push_back(*account);
          }
        }
      }
      MakeGroupFromCall(id, request->getString("title").value_or(""), accounts);
    } else if (cmd == "call_remove") {
      RemoveFromCall(id, request->getString("account").value_or(""));
    } else if (cmd == "call_add_guest_to_group") {
      AddGuestToGroup(id, request->getString("account").value_or(""));
    } else if (cmd == "call_group_invite") {
      Settle();
      const CallScreen& screen = presenter_.Screen();
      Object out = Reply(id, true);
      out.set("shown", screen.group_invite.has_value());
      out.set("nonce", screen.group_invite ? screen.group_invite->invite_id : std::string());
      out.set("title", screen.group_invite ? screen.group_invite->group_title : std::string());
      out.set("from", screen.group_invite ? screen.group_invite->maker_identity : std::string());
      Emit(out);
    } else if (cmd == "call_group_invite_join") {
      JoinCallGroupInvite(id);
    } else if (cmd == "call_group_invite_not_now") {
      Settle();
      if (!presenter_.Screen().group_invite) {
        ReplyError(id, "no group banner in this call");
      } else {
        presenter_.PutOffGroupInvite();
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
    } else if (cmd == "send_mentions") {
      SendMentions(id, *request);
    } else if (cmd == "pin" || cmd == "unpin" || cmd == "mute" || cmd == "unmute" || cmd == "archive" ||
               cmd == "unarchive" || cmd == "mark_unread" || cmd == "mark_read" || cmd == "mute_mentions" ||
               cmd == "allow_mentions") {
      // A chat row's menu (ChatMenus::SessionActions → ChatPresenter).
      const std::string thread = request->getString("thread").value_or("");
      if (cmd == "pin" || cmd == "unpin") {
        chats_.SetPinned(thread, cmd == "pin");
      } else if (cmd == "mute") {
        chats_.Mute(thread, SessionMuteChoice::Always);
      } else if (cmd == "unmute") {
        chats_.Unmute(thread);
      } else if (cmd == "mute_mentions" || cmd == "allow_mentions") {
        chats_.SetMuteMentions(thread, cmd == "mute_mentions");
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
      auto contact =
          facade_->FindContactByIdentity(request->getString("account").value_or(""), mesh::ContactIdKind::Account);
      if (!contact || !*contact) {
        ReplyError(id, "no such contact");
        return;
      }
      auto done = chats_.ShareContact(request->getString("thread").value_or(""), (*contact)->id);
      done ? Emit(Reply(id, true)) : ReplyError(id, done.error().message);
    } else if (cmd == "add_to_contacts") {
      // *Add to contacts* in the sheet of a direct chat with someone not in contacts (ChatPeerSheet →
      // ChatPresenter): the chat on screen, its peer added from what this device holds for them.
      const std::string thread_id = request->getString("thread").value_or("");
      auto thread = facade_->GetThread(thread_id);
      if (!thread || !*thread || !chats_.SelectThread(thread_id)) {
        ReplyError(id, "could not open the chat");
        return;
      }
      auto added = chats_.AddPeerToContacts((*thread)->peer_identity_value);
      if (!added) {
        ReplyError(id, added.error().message);
        return;
      }
      Object out = Reply(id, true);
      out.set("contact", *added);
      Emit(out);
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
      auto contact =
          facade_->FindContactByIdentity(request->getString("account").value_or(""), mesh::ContactIdKind::Account);
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
      auto done =
          chats_.ToggleReaction(request->getString("message").value_or(""), request->getString("emoji").value_or(""));
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
      chats_.DeleteForEveryone(
          message, [id](Roe<void> done) { done ? Emit(Reply(id, true)) : ReplyError(id, done.error().message); });
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
      // Close a chat the way its plan says (an owner names a `successor`, or dissolves with none).
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
          // No successor: the owner dissolves the group (the chat stays, read-only).
          done = successor.empty() ? chats_.DissolveGroup(plan.group_id)
                                   : chats_.HandOverAndClose(thread, plan.group_id, successor);
          break;
        case CloseChatPlan::Kind::Dissolve:
          done = chats_.DissolveGroup(plan.group_id);
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
        Hub().Apply(ProjectNotifications(prefs));
        Object out = Reply(id, true);
        out.set("stay_connected", AppLifecycle::StaysConnected());
        Emit(out);
      }
    } else if (cmd == "call_invite_policy") {
      // Me → Security → Who can call me (privacy P007; `contacts_only` for a new profile): the profile pref,
      // applied as ConfigApplyBridge applies a pref change.
      const std::string policy = request->getString("policy").value_or("");
      if (policy != "everyone" && policy != "contacts_only" && policy != "nobody") {
        ReplyError(id, "policy: everyone | contacts_only | nobody");
        return;
      }
      ProfilePreferences prefs = store_.Snapshot().profile_prefs;
      prefs.call_invite_policy = policy;
      if (auto saved = store_.SaveProfilePrefs(prefs); !saved) {
        ReplyError(id, saved.error().message);
      } else {
        Hub().Apply(ProjectPolicy(prefs));
        Object out = Reply(id, true);
        out.set("call_invite_policy", store_.Snapshot().profile_prefs.call_invite_policy);
        Emit(out);
      }
    } else if (cmd == "push_wake") {
      // A call push arrived (PushWakeJni: FCM → RequestUrgentWakeSync).
      BackgroundSyncScheduler::Instance().RequestUrgentWakeSync();
      Emit(Reply(id, true));
    } else if (cmd == "warm_seeds") {
      if (!Hub().IsMeshReady()) {
        ReplyError(id, "mesh not ready");
        return;
      }
      product_->Mesh().Connectivity().Rendezvous().WarmBootstrapSeedSessions();
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
      (void)product_.release(); // work still running may touch it
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
      row.set("kind", thread.kind == ThreadKind::Direct  ? "direct"
                      : thread.kind == ThreadKind::Group ? "group"
                                                         : "other");
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
        row.set("mentioned", shown.mentioned);
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
    presenter_.StartCall(
        thread_id, video,
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

  /** V062 3: a guest's "Remove from call…" (confirmed): the presenter's, checked by the stack. */
  void RemoveFromCall(int64_t id, const std::string& account) {
    if (account.empty()) {
      ReplyError(id, "account required");
      return;
    }
    presenter_.RemoveFromCall(account, [id](Roe<void> removed) {
      removed ? Emit(Reply(id, true)) : ReplyError(id, removed.error().message);
    });
  }

  /** V062 4: a guest's "Add {name} to the group": the group's ordinary invite, made in the call. */
  void AddGuestToGroup(int64_t id, const std::string& account) {
    if (account.empty()) {
      ReplyError(id, "account required");
      return;
    }
    presenter_.AddGuestToGroup(account, [id](Roe<void> invited) {
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

  /**
   * G019 / G020 6, a group chat's member-made changes: invite_to_group (the ordinary invite by account), rename_group,
   * remove_everyone_added_by {removed} and reinvite_everyone_removed_by {invited}. Invites seal and send on a worker
   * (as InviteMembersAsync does); a refusal comes back as the error, its message key kept.
   */
  void ChangeGroup(int64_t id, const std::string& cmd, const std::string& thread_id, const std::string& account,
                   const std::string& title) {
    auto thread = facade_->GetThread(thread_id);
    if (!thread || !*thread || !(*thread)->group_id) {
      ReplyError(id, "not a group chat");
      return;
    }
    const std::string group_id = *(*thread)->group_id;
    if (cmd == "rename_group") {
      auto renamed = facade_->RenameGroupShared(group_id, title);
      renamed ? Emit(Reply(id, true)) : ReplyError(id, renamed.error().message);
      return;
    }
    if (account.empty()) {
      ReplyError(id, "account required");
      return;
    }
    if (cmd == "remove_everyone_added_by") {
      auto removed = facade_->RemoveEveryoneAddedBy(group_id, account);
      if (!removed) {
        ReplyError(id, removed.error().message);
        return;
      }
      Object out = Reply(id, true);
      out.setJsonUInt("removed", static_cast<uint64_t>(*removed));
      Emit(out);
      return;
    }
    const bool reinvite = cmd == "reinvite_everyone_removed_by";
    AppRuntime::PostWorkerAndReplyOnUI<Roe<size_t>>(
        WorkerLane::Normal,
        [this, reinvite, group_id, account]() -> Roe<size_t> {
          if (reinvite) {
            return facade_->ReinviteEveryoneRemovedBy(group_id, account);
          }
          if (auto invited = facade_->InviteIdentity(group_id, account); !invited) {
            return invited.error();
          }
          return size_t{1};
        },
        [id, reinvite](Roe<size_t> invited) {
          if (!invited) {
            ReplyError(id, invited.error().message);
            return;
          }
          Object out = Reply(id, true);
          if (reinvite) {
            out.setJsonUInt("invited", static_cast<uint64_t>(*invited));
          }
          Emit(out);
        });
  }

  /** G021: the group's live invite links, as group info lists them. */
  Value LinkRows(const std::string& group_id) {
    std::vector<Value> links;
    for (const GroupLinkRow& link : facade_->GroupInviteLinks(group_id)) {
      Object row;
      row.set("link_id", link.link_id);
      row.set("maker", link.maker);
      row.set("expires_at", link.expires_at);
      row.setJsonUInt("max_uses", link.max_uses);
      row.setJsonUInt("uses", link.uses);
      links.push_back(ObjectValue(std::move(row)));
    }
    return ArrayValue(std::move(links));
  }

  /** G021: group info's "Invite link…" (a new link) and a link's "Revoke link". */
  void InviteLink(int64_t id, const std::string& cmd, const Object& request) {
    auto thread = facade_->GetThread(request.getString("thread").value_or(""));
    if (!thread || !*thread || !(*thread)->group_id) {
      ReplyError(id, "not a group chat");
      return;
    }
    const std::string group_id = *(*thread)->group_id;
    const std::string link_id = request.getString("link_id").value_or("");
    const uint64_t max_uses = request.getNonNegInt("max_uses").value_or(0);
    const int64_t lifetime_ms = static_cast<int64_t>(request.getNonNegInt("expires_in_s").value_or(0)) * 1000;
    const bool make = cmd == "make_invite_link";
    AppRuntime::PostWorkerAndReplyOnUI<Roe<GroupInviteLinkWorkflow::MadeLink>>(
        WorkerLane::Normal,
        [this, make, group_id, link_id, max_uses, lifetime_ms]() -> Roe<GroupInviteLinkWorkflow::MadeLink> {
          if (make) {
            return facade_->MakeInviteLink(group_id, max_uses, lifetime_ms);
          }
          if (auto revoked = facade_->RevokeInviteLink(group_id, link_id); !revoked) {
            return revoked.error();
          }
          return GroupInviteLinkWorkflow::MadeLink{};
        },
        [id, make](Roe<GroupInviteLinkWorkflow::MadeLink> done) {
          if (!done) {
            ReplyError(id, done.error().message);
            return;
          }
          Object out = Reply(id, true);
          if (make) {
            out.set("link", done->text);
            out.set("link_id", done->link_id);
            out.set("expires_at", done->expires_at);
          }
          Emit(out);
        });
  }

  /** G021: a link opened, pasted or scanned — the request to its maker, then its answer (the preview sheet's). */
  void JoinByLink(int64_t id, const std::string& link) {
    AppRuntime::PostWorkerAndReplyOnUI<Roe<GroupJoinState>>(
        WorkerLane::Normal,
        [this, link]() -> Roe<GroupJoinState> {
          auto request_id = facade_->AskToJoinByLink(link);
          if (!request_id) {
            return request_id.error();
          }
          // The answer, or none once the joiner stops waiting (group_link_join::kAnswerTimeoutMs).
          const int64_t until = util::NowUnixMs() + group_link_join::kAnswerTimeoutMs + 5'000;
          std::optional<GroupJoinState> state = facade_->JoinByLinkState(*request_id);
          while (state && state->phase == GroupJoinState::Phase::Asking && util::NowUnixMs() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            state = facade_->JoinByLinkState(*request_id);
          }
          if (!state) {
            return Error("the join was forgotten");
          }
          return *state;
        },
        [id](Roe<GroupJoinState> state) {
          if (!state) {
            ReplyError(id, state.error().message);
            return;
          }
          Object out = Reply(id, true);
          out.set("request_id", state->request_id);
          if (state->phase == GroupJoinState::Phase::Preview && state->preview) {
            Object preview;
            preview.set("title", state->preview->group_title);
            preview.setJsonUInt("member_count", state->preview->member_count);
            preview.set("group_id", state->preview->group_id);
            out.set("preview", std::move(preview));
          } else if (state->phase == GroupJoinState::Phase::Refused) {
            out.set("refused", group_link_join::RefusalToString(state->reason));
          } else {
            out.set("no_answer", true);
          }
          Emit(out);
        });
  }

  /** G021: the preview's Join. */
  void ConfirmJoin(int64_t id, const std::string& request_id) {
    AppRuntime::PostWorkerAndReplyOnUI<Roe<Thread>>(
        WorkerLane::Normal, [this, request_id]() { return facade_->ConfirmJoinByLink(request_id); },
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
        row.setJsonUInt("member_count", invite->member_count); // G019: 0 when the invite does not say
        rows.push_back(ObjectValue(std::move(row)));
      }
    }
    Object out = Reply(id, true);
    out.set("invites", ArrayValue(std::move(rows)));
    Emit(out);
  }

  /** Someone in a call by their account, as the picker names them: their contact, else their card's nickname. */
  std::string CallMemberName(const std::string& account) {
    if (auto contact = facade_->FindContactByIdentity(account, mesh::ContactIdKind::Account); contact && *contact) {
      if (std::string name = FormatContactTitle(**contact); !name.empty()) {
        return name;
      }
    }
    return Hub().KeptCardNickname(account);
  }

  /**
   * G022: the call screen's "Make a group…" (CallPresenter::OpenMakeGroup), then the picker's Create: everyone else in
   * the call picked, or just `accounts` (each must be in it); the title defaults to the names, as the picker's does.
   */
  void MakeGroupFromCall(int64_t id, std::string title, const std::vector<std::string>& accounts) {
    Settle();
    make_group_call_id_.clear();
    presenter_.OpenMakeGroup();
    const std::string call_id = make_group_call_id_;
    if (call_id.empty()) {
      ReplyError(id, "this call offers no Make a group");
      return;
    }
    std::vector<std::string> members;
    if (auto participants = calls_->ListJoinedParticipants(call_id)) {
      for (const CallParticipant& participant : *participants) {
        members.push_back(participant.identity);
      }
    }
    const std::string self = facade_->LoadProfileIdentityView().account_id;
    std::vector<std::string> picked = CallMakeGroupPick(members, self);
    if (!accounts.empty()) {
      for (const std::string& account : accounts) {
        if (std::find(picked.begin(), picked.end(), account) == picked.end()) {
          ReplyError(id, "not in the call: " + account);
          return;
        }
      }
      picked = accounts;
    }
    if (title.empty()) {
      std::vector<std::string> names;
      auto own = Hub().Identity().Get();
      names.push_back(own ? own->nickname : std::string());
      for (const std::string& account : picked) {
        names.push_back(CallMemberName(account));
      }
      title = DefaultCallGroupTitle(names, ", ", "Group chat");
    }
    AppRuntime::PostWorkerAndReplyOnUI<Roe<Thread>>(
        WorkerLane::Normal,
        [this, title, call_id, picked]() { return Hub().Groups().CreateGroupFromCall(title, call_id, picked); },
        [id, title, picked](Roe<Thread> thread) {
          if (!thread) {
            ReplyError(id, thread.error().message);
            return;
          }
          Object out = Reply(id, true);
          out.set("thread", thread->id);
          out.set("group_id", thread->group_id.value_or(""));
          out.set("title", title);
          std::vector<Value> invited;
          for (const std::string& account : picked) {
            invited.emplace_back(account);
          }
          out.set("invited", ArrayValue(std::move(invited)));
          Emit(out);
        });
  }

  /** G022: the call's group banner Join (CallPresenter::JoinGroupInvite → the ordinary accept). */
  void JoinCallGroupInvite(int64_t id) {
    Settle();
    if (!presenter_.Screen().group_invite) {
      ReplyError(id, "no group banner in this call");
      return;
    }
    const std::string group_id = [this]() {
      const std::string nonce = presenter_.Screen().group_invite->invite_id;
      for (const GroupInviteGate::InviteInCall& invite : Hub().GroupInvitesInCall(presenter_.Screen().call_id)) {
        if (invite.invite_nonce == nonce) {
          return invite.group_id;
        }
      }
      return std::string();
    }();
    presenter_.JoinGroupInvite([this, id, group_id](Roe<void> joined) {
      if (!joined) {
        ReplyError(id, joined.error().message);
        return;
      }
      Object out = Reply(id, true);
      out.set("group_id", group_id);
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
  /** Per chat, its last mention alert while muted (NoticeAlerts' rate limit). */
  std::map<std::string, int64_t> mention_alert_ms_;
  std::string last_kind_ = "none";
  std::string last_line_;
  std::string last_error_;
  /** G022: the call "Make a group…" was pressed in (its callback; the command that pressed it takes it). */
  std::string make_group_call_id_;
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
  std::string restore_phrase_file;
  std::string restore_saved_copy;
  bool synthetic_video = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--pin") == 0 && i + 1 < argc) {
      pin = argv[++i];
    } else if (std::strcmp(argv[i], "--log-file") == 0 && i + 1 < argc) {
      log_file = argv[++i];
    } else if (std::strcmp(argv[i], "--restore-phrase-file") == 0 && i + 1 < argc) {
      restore_phrase_file = argv[++i];
    } else if (std::strcmp(argv[i], "--restore-saved-copy") == 0 && i + 1 < argc) {
      restore_saved_copy = argv[++i];
    } else if (std::strcmp(argv[i], "--synthetic-video") == 0) {
      synthetic_video = true;
    }
  }
  if (pin.empty()) {
    std::fprintf(stderr,
                 "usage: pp-app-probe --config FILE --pin PIN [--log-file FILE] [--restore-phrase-file FILE] "
                 "[--restore-saved-copy FILE] [--synthetic-video]\n");
    return 2;
  }
  // No sound card in the lab: SDL's dummy driver plays into nothing and records silence, so the
  // real device path still runs.
  // (An SDL_AUDIO_DRIVER in the environment still wins over this hint.)
  SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
  if (synthetic_video) {
    // Before anything makes a codec or opens a device: every one from now on is the stand-in.
    pbr::SetPlatformVideoCodecOverride([]() { return pbr::SyntheticVideo::CreateCodec(); });
    (void)pbr::MediaDeviceArbiter::SetDefaultBackendFactory(
        []() { return pbr::SyntheticVideo::WithCamera(pbr::CreateSdlMediaDeviceBackend()); });
  }

  auto root = pbr::logging::getRootLogger();
  root.setLevel(pbr::logging::Level::INFO);
  if (!pbr::Platform::EarlyInit()) {
    std::fprintf(stderr, "platform early init failed\n");
    return 1;
  }
  if (!log_file.empty()) {
    pbr::LogFile::Install(log_file);
  }

  pbr::AppRuntimeConfig runtime = pbr::ProductRuntimeConfig();
  pbr::AppRuntime::Initialize(runtime);
  pbr::AppRuntime::InitializeUI();

  auto probe = std::make_unique<pbr::Probe>();
  probe->Wire();
  pbr::BootstrapOptions options;
  options.argc = argc;
  options.argv = argv;
  options.pin = pin;
  if (!restore_phrase_file.empty()) {
    std::ifstream phrase_in(restore_phrase_file);
    std::string phrase;
    std::getline(phrase_in, phrase);
    options.restore_recovery_phrase = std::move(phrase);
  }
  if (!restore_saved_copy.empty()) {
    std::ifstream copy_in(restore_saved_copy, std::ios::binary);
    options.restore_saved_copies.emplace_back(std::istreambuf_iterator<char>(copy_in),
                                              std::istreambuf_iterator<char>());
  }
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
  // work is posted, else after kLiveChromeFrameIntervalSec while a ring / call panel shows, or
  // kIdleFrameMaxWaitSec when idle. Each frame runs the UI tasks, then ticks (CallController::Tick).
  while (!probe->QuitRequested() && !stdin_closed) {
    const double wait_sec = probe->CallShown() ? pbr::kLiveChromeFrameIntervalSec : pbr::kIdleFrameMaxWaitSec;
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
  std::_Exit(0); // the stdin reader may still be blocked in getline
}
