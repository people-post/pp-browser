#pragma once

#include "common/Error.h"
#include "common/Module.h"
#include "lib/calls/CallScreen.h"
#include "feature/calls/CallFunctionalPorts.h"

#include <functional>
#include <memory>
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
  };
  Kind kind = Kind::Error;
  std::string key;
  std::string text;
  std::string identity;
};

/** What pressing a call button led to. */
struct CallStartResult {
  enum class Kind { Started, Joined, PickPeople, Failed };
  Kind kind = Kind::Failed;
  std::string call_id;
  /** Failed: an i18n key (`call.error.*`), or the stack's message in `error_text`. */
  std::string error_key;
  std::string error_text;
};

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
    // Moments the call metrics follow.
    std::function<void(const std::string& call_id, bool video)> on_ring_started;
    std::function<void(const std::string& call_id, bool video)> on_outbound_started;
    std::function<void(const std::string& call_id, bool voice_only)> on_accept;
    std::function<void(const std::string& call_id)> on_decline;
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
  /** The in-call "add people" button. */
  void OpenAddPeople();
  /** The add-people picker's choice: each is invited (`done` once per invitee). */
  void InviteToActiveCall(const std::vector<std::string>& identities, std::function<void(Roe<void>)> done = {});
  bool VideoCallAvailable();
  /** The ring's answer buttons. Return the call answered (empty: nothing ringing). */
  std::string Accept(bool voice_only);
  std::string AcceptWithCharge();
  std::string Decline();
  /** End. Returns the call left (empty: none — a stale button only clears the screen). */
  std::string Leave();
  void Retry();
  void ToggleMute();
  void ToggleCamera();
  void ToggleSpeaker();

private:
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
  bool ring_shown_ = false;
  /**
   * The ring the user just answered / declined: the stack's state reads Ringing for it until the
   * owner has taken the click, and showing it again then flashed the ring after Accept.
   */
  std::string answered_call_id_;
  std::string declined_call_id_;
  /**
   * 2026-09-28: video calls start with the local camera on — set by the caller (video StartCall)
   * or the callee (non-voice-only accept of a video call); consumed once the call is connected.
   * Keyed by call id, so a stale one never fires for another call.
   */
  bool auto_camera_pending_ = false;
  std::string auto_camera_call_id_;
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
