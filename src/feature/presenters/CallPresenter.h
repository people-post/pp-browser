#pragma once

#include "common/Error.h"
#include "common/Module.h"
#include "lib/calls/CallScreen.h"
#include "lib/media/IMediaView.h"
#include "feature/calls/CallFunctionalPorts.h"
#include "feature/calls/CallUiBackend.h"
#include "feature/presenters/CallBackOffer.h"

#include <functional>
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
    /** A media path failure the user should be able to read: stays up (`text`). */
    StickyError,
    /** The peer ended (or declined) a call this device placed or was in: `identity`. */
    PeerEnded,
    PeerDeclined,
    /** A call rings (`identity`: the caller); the platform notifies when the user is not looking. */
    IncomingCall,
    /** V062 1: `identity` joined the group's call as `brought_by`'s guest (once per guest and call). */
    GuestJoined,
    /** V064: the 1:1 call End & accept ended (`ending`) can be called back now that the call taken is over. */
    CallBack,
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
    /** P017: the calls layer learned `call_id` is over (each once): the phone's call screen lets a push's call go. */
    std::function<void(const std::string& call_id)> on_call_over;
    /** A call button was pressed in a call to another chat: asks "end your current call?"; `answer(true)` = go on. */
    std::function<void(std::function<void(bool)> answer)> confirm_end_current_call;
    /** A call button was pressed in the chat of the call already up: bring its screen back (from the minimised chip). */
    std::function<void()> show_current_call;
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

  const CallScreen& Screen() const { return screen_; }
  /** The call this device is in (empty: none). */
  const std::string& ActiveCallId() const { return active_call_id_; }
  /** The call placed or answered with video (its camera comes on once connected; "Starting video…"). */
  const std::string& VideoIntentCallId() const { return auto_camera_call_id_; }
  CallUiBackend* Backend() const;

  // --- buttons ----------------------------------------------------------------------------------

  /** A chat's call button: a 1:1 call, the group's call to join, or the member picker. */
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
  /** The camera did not come on (or went off): tell the user in plain words; the detail is logged only. */
  void ReportCameraFailure(const CameraFailure& failure);

private:
  /**
   * A call button while a call is up: another chat's call asks first (then leaves it and runs `place` again), the
   * same chat's call is shown, no question. True when handled here (`place` is for the Place case only).
   */
  bool GuardNewCall(const std::string& thread_id, const std::function<void()>& place, const StartDone& done);
  void RequestStart(const std::string& thread_id, bool video, const std::vector<std::string>& invitees,
                    StartDone done);
  std::string RingCallId(CallUiBackend& backend) const;
  void Fail(const std::string& key, const StartDone& done = {});
  void FailText(const std::string& text);
  void Notify(CallNotice notice);
  void Changed();
  /** True when the peer's end of a call was announced. */
  bool TakeStackNotices(CallUiBackend& backend);
  void FillInCall(CallScreen& s, CallUiBackend& backend, const std::string& call_id, bool video, bool group_call);
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
  bool MessagingInitialized() const;

  CallFunctionalPorts ports_;
  Callbacks callbacks_;
  CallScreen screen_;
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
