#pragma once

#include "common/Error.h"
#include "common/Module.h"
#include "lib/calls/CallScreen.h"
#include "lib/media/IMediaView.h"
#include "feature/calls/CallFunctionalPorts.h"
#include "feature/calls/CallUiBackend.h"
#include "feature/presenters/CallBackOffer.h"
#include "feature/presenters/CallMediaFrame.h"
#include "feature/presenters/CallSpeakerButton.h"
#include "feature/presenters/CallWakePlaceholder.h"
#include "foundation/platform/PendingNotificationTap.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

class CallUiBackend;

/** Something to tell the user once (a toast, a banner, a notification); the renderer words it. */
struct CallNotice {
  enum class Kind {
    /** A call action failed: `key` (an i18n key) or the stack's `text`. */
    Error,
    /** The peer ended (or declined) a call this device placed or was in: `identity`. */
    PeerEnded,
    PeerDeclined,
    /** A call rings (`identity`: the caller); the platform notifies when the user is not looking. */
    IncomingCall,
    /** V062 1: `identity` joined the group's call as `brought_by`'s guest (once per guest and call). */
    GuestJoined,
    /** V064: the 1:1 call End & accept ended (`ending`) can be called back now that the call taken is over. */
    CallBack,
    /** "Who can call me" dropped a call from `identity`: `key` (an i18n key) says which setting did (the pill). */
    CallBlocked,
  };
  Kind kind = Kind::Error;
  std::string key;
  std::string text;
  std::string identity;
  std::string brought_by;
  /** IncomingCall: it rings over a call this device is in, and answering ends `ending` (V064). CallBack: the call. */
  bool conflict = false;
  CallScreen::EndingCall ending;
};

/** What pressing a call button led to. */
struct CallStartResult {
  enum class Kind { Started, Joined, PickPeople, Cancelled, Failed };
  Kind kind = Kind::Failed;
  std::string call_id;
  /** Failed: an i18n key (`call.error.*`), or the stack's message in `error_text`. */
  std::string error_key;
  std::string error_text;
};

/**
 * A group chat's "a call goes on here" bar (DEV/PLAN-ui-batch8-group-pages.md 4): whether it shows, its words (an
 * i18n key, voice or video) and how many have joined (0: unknown, so no number shows; never "0 people").
 */
struct CallJoinBar {
  bool shown = false;
  bool video = false;
  int joined = 0;
  /** chat.call_bar.voice / chat.call_bar.video; "" when not shown. */
  std::string text_key;

  bool operator==(const CallJoinBar&) const = default;
};
CallJoinBar MakeCallJoinBar(const std::optional<CallUiBackend::JoinableCallInfo>& call);

/**
 * A call from a chat (CallPresenter::CallFromChat): whether it asks for video. A call that goes on in the chat is
 * joined as it is (no voice / video choice); video this device cannot send goes voice; else as asked.
 */
bool ChatCallAsksVideo(bool call_joinable, bool want_video, bool video_available);

/**
 * The call surface's decisions and the call buttons, without a GUI: which call shows (ring,
 * answering, in call, nothing), what its status line says, which buttons it offers, and what each
 * button does. CallController renders the screen (words, avatars, video, ringtone); pp-app-probe
 * presses the same buttons and reports the same screen, so the app lab checks what people see.
 *
 * UI thread (its own posts move work there). Results of async actions for a destroyed presenter are
 * dropped.
 */
class CallPresenter : public Module {
public:
  struct Callbacks {
    /** The screen may have changed: re-read Screen(). */
    std::function<void()> on_changed;
    std::function<void(const CallNotice&)> notify;
    /** A group's call button with no call to join: the member picker places the call. */
    std::function<void(const std::string& thread_id, bool video)> open_group_picker;
    /** The in-call "add people" button. */
    std::function<void(const std::string& call_id)> open_add_people;
    /** G022: "Make a group…": the people picker, everyone in the call `call_id` picked. */
    std::function<void(const std::string& call_id)> open_make_group;
    // Moments the call metrics follow.
    std::function<void(const std::string& call_id, bool video)> on_ring_started;
    std::function<void(const std::string& call_id, bool video)> on_outbound_started;
    std::function<void(const std::string& call_id, bool voice_only)> on_accept;
    std::function<void(const std::string& call_id)> on_decline;
    /** A call button was pressed in a call to another chat: asks "end your current call?"; `answer(true)` = go on. */
    std::function<void(std::function<void(bool)> answer)> confirm_end_current_call;
    /** A call button was pressed in the chat of the call already up: bring its screen back (from the minimised chip). */
    std::function<void()> show_current_call;
    /** V060: the OS has this device's call audio (the GUI's CallLocalHold); none: never on hold here. */
    std::function<bool()> on_hold_here;
  };
  using StartDone = std::function<void(const CallStartResult&)>;

  CallPresenter();

  void BindPorts(CallFunctionalPorts ports);
  void BindCallbacks(Callbacks callbacks);

  /** (Re)bind ring callbacks when the session manager changes (it is rebuilt across unlock). */
  void BindToMessaging();
  /** Every frame: expire rings that ran out; the rest is event-driven. */
  void Tick();
  /** Decide the screen from the call stack now (after inbox sync, a ring change, an action). */
  void Refresh();
  /** The app was woken for a call: look for its ring now. */
  void OnCallWake();
  /**
   * The vault is unlocked and the messaging stack is up (ConversationsHub::EnsureMessagingReady finished): a woken
   * app's inbox sync can run now. Only then does the placeholder's "no ring" clock start. Unlock happens once per run
   * (the app never re-locks while running), so it is latched.
   */
  void NoteMessagingReady();
  /** What the GUI does with the "Incoming call, connecting…" ring page this step. */
  enum class WakeRing { Keep, Show, Clear };
  struct WakeStep {
    /** Show: put the page up (once it is up, nothing more); Clear: take it down quietly; Keep: leave the ring alone. */
    WakeRing ring = WakeRing::Keep;
    /** Set on the one step where the placeholder ends; the two fields below go with it. */
    CallWakePlaceholder::Outcome outcome = CallWakePlaceholder::Outcome::None;
    int64_t shown_ms = 0;
    std::optional<int64_t> ready_ms;
  };
  /**
   * The incoming-call placeholder (CallWakePlaceholder) for the latest call-alert tap (PendingNotificationTap) and the
   * app's visibility. Works with no backend (a locked vault). "Messaging ready" = NoteMessagingReady (after unlock); "no
   * call screen" = our own screen_. `phone_layout`: only phones have the page (anything else clears one that is up).
   */
  WakeStep StepWakePlaceholder(bool phone_layout,
                               PendingNotificationTap::Clock::time_point now = PendingNotificationTap::Clock::now());
  /**
   * "Who can call me" dropped an invite (DEV/PLAN-call-stranger-declined.md); each call id once. A 1:1 call is
   * recorded in the chat with its caller (ports' record_blocked_call); unless it was too old to ring, the
   * connecting page waiting for it closes (StepWakePlaceholder: Blocked, with the pill), else the pill shows to a
   * user looking at the app, once a minute per caller. A group's call, a caller the user blocked, our own account:
   * nothing. A ring on screen is never touched.
   */
  void OnInviteBlocked(const BlockedCallInvite& blocked,
                       PendingNotificationTap::Clock::time_point now = PendingNotificationTap::Clock::now());
  /** One pill per caller in this long, except the one that explains a closed (or never shown) connecting page. */
  static constexpr int64_t kBlockedPillPerCallerMs = 60'000;

  const CallScreen& Screen() const { return screen_; }
  /**
   * The media decisions of the call on screen, stepped with each refresh (CallMediaFrame): the status line, whether
   * it reconnects (the capsule), the video stage, the one-way-audio hint and the connection story.
   */
  const CallMediaScreen& MediaScreen() const { return media_frame_.View(); }
  /** The call this device is in (empty: none). */
  const std::string& ActiveCallId() const { return active_call_id_; }
  /** The call placed or answered with video (its camera comes on once connected; "Starting video…"). */
  const std::string& VideoIntentCallId() const { return auto_camera_call_id_; }
  CallUiBackend* Backend() const;

  // --- buttons ----------------------------------------------------------------------------------

  /**
   * The one way a chat calls (ChatCallAsksVideo, then StartCall): the chat's call button, a tapped call record (#164)
   * and "Call {name} back?" (V064). False when nothing was asked (no chat).
   */
  bool CallFromChat(const std::string& thread_id, bool want_video, StartDone done = {});
  /** A 1:1 call, the group's call to join, or the member picker — `video` as given (CallFromChat decides it). */
  bool StartCall(const std::string& thread_id, bool video, StartDone done = {});
  /** The member picker's Call (or a 1:1 with its peer). */
  bool StartCallWithInvitees(const std::string& thread_id, bool video, const std::vector<std::string>& invitees,
                             StartDone done = {});
  /** A group call goes on in this chat that this device may join (the button reads "Join"). */
  bool CallJoinableInThread(const std::string& thread_id) const;
  /** The bar over a group chat while a call there may be joined (read now, through CallUiBackend). */
  CallJoinBar JoinBarInThread(const std::string& thread_id) const;
  /** The in-call "add people" button. */
  void OpenAddPeople();
  /** The add-people picker's choice: each is invited (`done` once per invitee). */
  void InviteToActiveCall(const std::vector<std::string>& identities, std::function<void(Roe<void>)> done = {});
  /**
   * V062 2: whether the add-people picker offers `identity` for the active call: in a group's call, someone outside
   * the group only to a member allowed to bring guests (calls with no group: anyone).
   */
  bool MayAddToActiveCall(const std::string& identity) const;
  /** V062 3: "Remove from call…" on a guest (Screen() member's can_remove), once the user confirmed. */
  void RemoveFromCall(const std::string& identity, std::function<void(Roe<void>)> done = {});
  /** V062 4: "Add {name} to the group" on a guest (can_add_to_group): the group's ordinary invite, in this call. */
  void AddGuestToGroup(const std::string& identity, std::function<void(Roe<void>)> done = {});
  /** G022: "Make a group…" (shown by Screen().show_make_group). */
  void OpenMakeGroup();
  /** G022: the group banner's Join: the invite on it is accepted (the ordinary accept); the call goes on as it is. */
  void JoinGroupInvite(std::function<void(Roe<void>)> done = {});
  /** G022: the banner's Not now: it goes away in this call; the invite stays waiting (its card in the chat stays). */
  void PutOffGroupInvite();
  bool VideoCallAvailable();
  /** The ring's answer buttons. Return the call answered (empty: nothing ringing). */
  /**
   * Answer the ring: `call_id`, the one the phone's call screen answered, else the ring on screen. The click frame
   * already shows it answering (a redraw inside Accept must not read as the ring gone).
   */
  std::string Accept(bool voice_only, const std::string& call_id = {});
  std::string AcceptWithCharge();
  std::string Decline();
  /** End. Returns the call left (empty: none — a stale button only clears the screen). */
  std::string Leave();
  void Retry();
  void ToggleMute();
  void ToggleCamera();
  /** Phones: front / rear camera while it is on. */
  void FlipCamera();
  void ToggleSpeaker();
  /** What the speaker button shows (CallSpeakerButtonOn): a tap being applied, else the OS route. */
  bool SpeakerShownOn() const;
  /**
   * Developer builds only (the app layer sets it under kDevToolsBuild; empty otherwise): true makes the next speaker
   * tap change the route the old way — before the mic closes — so the OS takes the recording stream and the media
   * engine's audio rebuild has to bring the mic back (Android merge check).
   */
  void SetDevRouteReorderSkip(std::function<bool()> skip);
  /**
   * Developer builds only (debug.pp.detach_call_ui): true makes each refresh end with the call id dropped while the
   * engine keeps the call — what CallUiAttachGuard is there for (tried on a device).
   */
  void SetDevDetachUi(std::function<bool()> detach);
  /** The camera did not come on (or went off): tell the user in plain words; the detail is logged only. */
  void ReportCameraFailure(const CameraFailure& failure);

private:
  /**
   * A call button while a call is up: another chat's call asks first (then leaves it and runs `place` again), the
   * same chat's call is shown, no question. True when handled here (`place` is for the Place case only).
   */
  bool GuardNewCall(const std::string& thread_id, const std::function<void()>& place, const StartDone& done);
  void RequestStart(const std::string& thread_id, bool video, const std::vector<std::string>& invitees, StartDone done);
  std::string RingCallId(CallUiBackend& backend) const;
  void Fail(const std::string& key, const StartDone& done = {});
  void FailText(const std::string& text);
  void Notify(CallNotice notice);
  void Changed();
  /** True when the peer's end of a call was announced. */
  bool TakeStackNotices(CallUiBackend& backend);
  void FillInCall(CallScreen& s, CallUiBackend& backend, const std::string& call_id, bool video, bool group_call);
  /** The store shows no call while the engine has one open: `next` keeps (or takes) the engine's call. */
  void KeepEngineCall(CallScreen& next, CallUiBackend& backend);
  void MaybeAutoCamera(CallUiBackend& backend, const CallScreen& s);
  /** B97: a live group call where nothing arrives from anyone for 30 s is left (once told). */
  void WatchForNoMedia(CallUiBackend& backend, const CallScreen& s, int64_t now);
  /** G022: the group banner of the call on `s` (the waiting invites read at most once a second). */
  void FillGroupInvite(CallScreen& s, int64_t now);
  /** V062: who is a guest in the group's call on `s`, what this device may do about them, and Add people. */
  void FillGuests(CallScreen& s, CallUiBackend& backend, const std::string& group_id, const std::string& local);
  /** V062 5: the ring on `s` brings this device in as a guest: how many it shows them to. */
  void FillGuestRing(CallScreen& s, CallUiBackend& backend, const PendingCallInvite& invite, const std::string& local);
  /** A ring over `active`: answering ends it (who with, for the ring's words and the call-back offer). */
  static void FillEndingCall(CallScreen& s, CallUiBackend& backend, const CallSession& active);
  /** V064: the call End & accept ended, offered again once the call taken is over. */
  void OfferCallBack(const CallScreen& next, const CallUiBackend::Surface& surface);
  /** V062 1: "{name} joined as {bringer}'s guest", once per guest of the call on `s`. */
  void AnnounceGuests(const CallScreen& s);
  /** The media frame of the screen `next` (in a call: its facts read, then acted on; answering: the story only). */
  void StepMedia(CallUiBackend& backend, const CallScreen& next, int64_t now);
  /** One tick's media facts for the call on `s`, after the engine's video upkeep. */
  CallMediaFacts ReadMediaFacts(CallUiBackend& backend, const CallScreen& s, int64_t now);
  /** What the frame asks of the call: the camera's failure told, a narrowed call's camera off, a dropped picture. */
  void ActOnMedia(CallUiBackend& backend, const CallMediaScreen& frame);
  /** The peer's camera (roster / frames) and the story's new moments, each change logged once. */
  void LogMedia(const CallMediaFacts& facts, const CallMediaScreen& frame);
  bool MessagingInitialized() const;

  CallFunctionalPorts ports_;
  /** The last speaker tap handed to the media engine as a route change (CallSpeakerButtonOn). */
  std::optional<CallSpeakerTap> speaker_tap_;
  std::function<bool()> dev_route_reorder_skip_;
  std::function<bool()> dev_detach_ui_;
  /** The call debug.pp.detach_call_ui last dropped (logged once per call). */
  std::string dev_detached_call_id_;
  /** The call KeepEngineCall last logged (once per call). */
  std::string kept_call_logged_;
  Callbacks callbacks_;
  CallScreen screen_;
  CallWakePlaceholder wake_placeholder_;
  /** The last StepWakePlaceholder asked for the page to show (so it is asked to clear it once). */
  bool wake_ring_shown_ = false;
  /** OnInviteBlocked: call ids already handled, and when (steady ms; kept an invite's lifetime). */
  std::map<std::string, int64_t> blocked_calls_;
  /** Dropped 1:1 calls that may still be a tapped alert's: the connecting page for one closes (taken once). */
  std::map<std::string, BlockedCallInvite> wake_blocks_;
  /** The last pill per caller (steady ms). */
  std::map<std::string, int64_t> blocked_pill_at_;
  /** The pill saying which setting dropped a call from `blocked`'s caller. */
  void NotifyBlocked(const BlockedCallInvite& blocked);
  /** Forget handled calls past an invite's lifetime, and per-caller pills past their minute. */
  void PruneBlocked(int64_t now_ms);
  bool messaging_ready_ = false;
  NoMediaWatch no_media_watch_;
  int64_t no_media_checked_ms_ = 0;
  /** Last CallSessionManager identity the ring callbacks were installed on. */
  const void* bound_calls_ = nullptr;
  std::string ringing_call_id_;
  std::string active_call_id_;
  /** The group of the active call (empty: none). */
  std::string active_group_id_;
  /** V062 1: the guests announced in the call `announced_call_id_`. */
  std::string announced_call_id_;
  std::set<std::string> announced_guests_;
  bool ring_shown_ = false;
  /**
   * The ring the user just answered / declined: the stack's state reads Ringing for it until the
   * owner has taken the click, and showing it again then flashed the ring after Accept.
   */
  std::string answered_call_id_;
  CallBackOffer call_back_;
  CallMediaFrame media_frame_;
  /** What LogMedia last logged: the peer's camera ("roster/frames"), and the story's moments of `story_logged_call_`. */
  std::string peer_video_log_key_;
  std::string story_logged_call_;
  size_t story_events_logged_ = 0;
  std::string declined_call_id_;
  /**
   * 2026-09-28: video calls start with the local camera on — set by the caller (video StartCall)
   * or the callee (non-voice-only accept of a video call); consumed once the call is connected.
   * Keyed by call id, so a stale one never fires for another call.
   */
  bool auto_camera_pending_ = false;
  std::string auto_camera_call_id_;
  /** G022: the invites made in the call `group_invites_call_id_`, as last read; and those put off ("Not now"). */
  std::string group_invites_call_id_;
  std::vector<CallGroupInvite> group_invites_;
  int64_t group_invites_read_ms_ = 0;
  std::set<std::string> group_invites_put_off_;
  /** A Join in flight: its banner stays hidden meanwhile. */
  std::string group_invite_joining_;
  std::shared_ptr<int> alive_ = std::make_shared<int>(0);
  template <typename Fn>
  auto WhileAlive(Fn fn) {
    return [weak = std::weak_ptr<int>(alive_), fn = std::move(fn)](auto&&... args) {
      if (weak.lock()) {
        fn(std::forward<decltype(args)>(args)...);
      }
    };
  }
};

} // namespace pbr
