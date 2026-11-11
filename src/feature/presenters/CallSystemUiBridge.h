#pragma once

#include "lib/calls/CallScreen.h"
#include "foundation/platform/CallSystemUi.h"
#include "foundation/platform/DeviceScreenState.h"
#include "feature/presenters/CallRingHandBack.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Keeps the phone's own call screen (ICallSystemUi: Android's incoming-call notification) in step with CallPresenter's screen, and turns what the user does there into the presenter's
 * buttons (P014).
 *
 * - A ring goes to the system screen when the user cannot see the app (not in the foreground, the screen dark, the
 *   phone locked, PP behind its PIN, or the platform could not say), or when a call push already put "Incoming call"
 *   there (the push's call is filled in with the ring's caller).
 * - Rung there, it comes back to the app's own ring page once the user can see the app again for
 *   kCallRingHandBackSettleMs (Reevaluate, every 300 ms while the system screen rings for us): the app's page and
 *   ringtone first, then the notification goes (never a decline). If the user leaves the app while it still rings, it
 *   goes to the system screen again — without raising the app this time (only a call's first notification does).
 *   DEV/PLAN-android-incoming-call-screen.md.
 * - Answered there, the call stays there until it ends; answered in the app, the system screen lets
 *   it go (the app's own call screen shows it).
 * - Answered or declined on the system screen before the ring was known (the push's call; the app
 *   was still waking, or locked): done to the first ring once it shows (the push names no call). The
 *   window is kPendingChoiceMs counted from the later of the choice and OnMessagingReady (a PIN may
 *   come first), and never longer than kPendingMaxMs after the choice.
 * - A ring over a call this device is in (the screen's `conflict`) never goes there (V064): answering it ends
 *   that call, which the system screen's Answer would not say. The app's ring says it (End & accept), and so
 *   does its alert in the background. The push's call is let go, and an answer given on it is not applied
 *   (a decline still is).
 *
 * UI thread. Holds no GUI: CallController wires it to the presenter.
 */
class CallSystemUiBridge {
public:
  struct Actions {
    /** Answer `call_id` (the ring the system screen answered). */
    std::function<void(const std::string& call_id, bool voice_only)> accept;
    std::function<void()> decline;
    std::function<void()> leave;
    std::function<std::string(const std::string& identity)> display_name;
    /** Unix ms (tests set it). */
    std::function<int64_t()> now_ms;
    /**
     * Where the ring shows changed outside a frame (Reevaluate): redraw now. On a hand-back it runs before the
     * notification goes, with SystemRings already false, so the app's ring page and ringtone come up first.
     */
    std::function<void()> ring_moved;
    /** The screen on / phone locked, asked each time; nothing: could not ask (not seen). Unset: on, unlocked. */
    std::function<std::optional<DeviceScreenState>()> screen_state;
    /** PP's PIN gate is up. Unset: never. */
    std::function<bool()> pin_locked;
  };
  static constexpr int64_t kPendingChoiceMs = 60'000;
  static constexpr int64_t kPendingMaxMs = 180'000;
  /**
   * Answered on the system screen, the call stays there while the app takes it: until a frame shows it answering or
   * in the call, at most this long. Accept redraws at once (re-entering Sync inside the system screen's answer), and
   * a frame between the ring and "answering" must not end the call the user just answered.
   */
  static constexpr int64_t kAnswerSettleMs = 5'000;

  CallSystemUiBridge(ICallSystemUi& ui, Actions actions);
  ~CallSystemUiBridge();
  CallSystemUiBridge(const CallSystemUiBridge&) = delete;
  CallSystemUiBridge& operator=(const CallSystemUiBridge&) = delete;

  /** After every change of the presenter's screen. `app_foreground`: the app's screen is up (AppLifecycle::IsVisible). */
  void Sync(const CallScreen& screen, const std::string& active_call_id, bool app_foreground);
  /**
   * Is the ring where the user can see it? Called every 300 ms while WantsReevaluation(), and on foreground: a ring the
   * system screen rings for us goes back to the app once the user has seen the app for kCallRingHandBackSettleMs; a
   * ring the app rings goes to the system screen once they cannot see it. Redraws through Actions::ring_moved.
   */
  void Reevaluate(bool app_foreground);
  /** The system screen rings a call for us: the 300 ms re-evaluation runs (it stops once the ring is over or back). */
  bool WantsReevaluation() const;
  /** The system screen rings `call_id` (its own ringtone): the app's stays quiet. */
  bool SystemRings(const std::string& call_id) const;
  /**
   * A ring showing now goes to the system screen (so the app posts no "Incoming call" banner). `over_call`: it
   * rings over a call this device is in (answering ends that one): never.
   */
  bool TakesRing(bool app_foreground, bool over_call) const;

  /** Messaging is up (after the PIN): the pending choices' window counts from now. */
  void OnMessagingReady();

  // ICallSystemUi::Callbacks (public for tests).
  void OnAnswer(const std::string& call_id);
  void OnEnd(const std::string& call_id);
  void OnPendingGone();

private:
  int64_t Now() const;
  bool Ringing(const std::string& call_id) const;
  /** A choice made at `at_ms` on the push's call is still good for the ring found now. */
  bool ChoiceLive(int64_t at_ms) const;
  /** A pending answer (or decline) of the push's call. */
  void RecordChoice(bool answer);
  void ClearChoice();
  /** Declines the ring: the user declined its call on the system screen before it showed. */
  void ApplyDecline();
  /**
   * A ring not seen before: the system screen's availability is asked again (the user may have turned call
   * notifications off since); the frames in between read what the platform keeps.
   */
  void RecheckOnNewRing(const CallScreen& screen);
  /** A ring over a call: the app rings it (V064); the push's call goes, a choice made on it is spent. */
  void LeaveRingToApp(const std::string& ring_call_id);
  /** The call on the system screen was answered there; tell the presenter to accept it. */
  void AcceptShown(bool voice_only);
  /** A frame without the shown call: it is over, unless the app is still taking a call answered there. */
  bool StillTaking() const;
  /** What the user can see now: the platform asked afresh (only when the app is in front). */
  CallRingUi UiNow(bool app_foreground) const;
  /** The user can see the app's ring page now (CallRingUiVisible of UiNow). */
  bool UiVisible(bool app_foreground) const;
  /** Puts `screen`'s ring on the system screen (or declines it: declined there before it showed). True: it rings there. */
  bool TakeRing(const CallScreen& screen);
  /** The app's ring page and ringtone first (ring_moved), then the notification goes. */
  void HandBack();

  ICallSystemUi& ui_;
  Actions actions_;
  CallScreen screen_;
  std::string active_call_id_;
  /** The call on the system screen; empty: none. */
  std::string shown_;
  /** Answered on the system screen (it keeps the call until it ends). */
  bool shown_answered_ = false;
  /** When it was answered there, and whether a frame has shown the app taking it since (answering, in the call). */
  int64_t answered_at_ms_ = 0;
  bool shown_taken_ = false;
  /** The push's call was answered / declined before its ring showed (0: no such choice). */
  int64_t answer_pending_at_ms_ = 0;
  int64_t decline_pending_at_ms_ = 0;
  /** The last ring RecheckOnNewRing asked about. */
  std::string rechecked_ring_call_id_;
  /** Messaging came up at this time (0: not seen). */
  int64_t ready_at_ms_ = 0;
  /** Inside HandBack's redraw: SystemRings is false while the notification is still up. */
  bool handing_back_ = false;
  /** Since when the hand-back's conditions have held (Reevaluate's samples); nothing: they do not now. */
  std::optional<int64_t> visible_since_ms_;
  /** The call whose notification was posted with the full-screen intent: later ones for it are plain. */
  std::string full_screen_call_id_;
};

} // namespace pbr
