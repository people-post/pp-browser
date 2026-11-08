#pragma once

#include "lib/calls/CallScreen.h"
#include "foundation/platform/CallSystemUi.h"

#include <cstdint>
#include <functional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Keeps the phone's own call screen (ICallSystemUi: Android's incoming-call notification) in step with CallPresenter's screen, and turns what the user does there into the presenter's
 * buttons (P014).
 *
 * - A ring goes to the system screen when the app is not in the foreground, or when a call push
 *   already put "Incoming call" there (the push's call is filled in with the ring's caller).
 * - Answered there, the call stays there until it ends; answered in the app, the system screen lets
 *   it go (the app's own call screen shows it).
 * - Answered or declined on the system screen before the ring was known (the push's call; the app
 *   was still waking, or locked): done to the ring once it shows — the ring of the call the push named
 *   (its `wake_hint`), or, for a push that named none, the first ring. The window is kPendingChoiceMs
 *   counted from the later of the choice and OnMessagingReady (a PIN may come first), and never
 *   longer than kPendingMaxMs after the choice.
 * - A ring over a call this device is in (the screen's `conflict`) never goes there (V064): answering it ends
 *   that call, which the system screen's Answer would not say. The app's ring says it (End & accept), and so
 *   does its alert in the background. The push's call is let go, and an answer given on it is not applied
 *   (a decline still is).
 * - The push's call waiting for its ring goes as soon as the app learns the call it names is over (OnCallOver: its
 *   invite read too late to ring, or for a call already ended; the call ended here): the caller gave up, nothing
 *   will ring (P017).
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
    std::function<void()> toggle_mute;
    std::function<std::string(const std::string& identity)> display_name;
    /** Unix ms (tests set it). */
    std::function<int64_t()> now_ms;
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

  /** After every change of the presenter's screen. */
  void Sync(const CallScreen& screen, const std::string& active_call_id, bool app_foreground);
  /** The system screen rings `call_id` (its own ringtone): the app's stays quiet. */
  bool SystemRings(const std::string& call_id) const;
  /**
   * A ring showing now goes to the system screen (so the app posts no "Incoming call" banner). `over_call`: it
   * rings over a call this device is in (answering ends that one): never.
   */
  bool TakesRing(bool app_foreground, bool over_call) const;

  /** Messaging is up (after the PIN): the pending choices' window counts from now. */
  void OnMessagingReady();
  /**
   * The calls layer learned `call_id` is over (its invite too late to ring or for a call already ended, or the call
   * ended): the push's call waiting for that call's ring goes now, not at its timeout (P017).
   */
  void OnCallOver(const std::string& call_id);

  // ICallSystemUi::Callbacks (public for tests).
  void OnAnswer(const std::string& call_id);
  void OnEnd(const std::string& call_id);
  void OnSetMuted(const std::string& call_id, bool muted);
  void OnPendingGone();

private:
  int64_t Now() const;
  bool Ringing(const std::string& call_id) const;
  /** A choice made at `at_ms` on the push's call is still good for the ring found now. */
  bool ChoiceLive(int64_t at_ms) const;
  /** The pending choice may be applied to the ring of `call_id` (it named that call, or none). */
  bool ChoiceFits(const std::string& call_id) const;
  /** A pending answer (or decline) of the push's call `call_id` (empty: the push named none). */
  void RecordChoice(bool answer, const std::string& call_id);
  void ClearChoice();
  /** Declines the ring: the user declined its call on the system screen before it showed. */
  void ApplyDecline();
  /** A ring over a call: the app rings it (V064); the push's call goes, a choice made on it is spent. */
  void LeaveRingToApp(const std::string& ring_call_id);
  /** The call on the system screen was answered there; tell the presenter to accept it. */
  void AcceptShown(bool voice_only);
  /** A frame without the shown call: it is over, unless the app is still taking a call answered there. */
  bool StillTaking() const;

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
  /** The call that choice was made on (the push's `wake_hint`); empty: the push named none, the first ring. */
  std::string choice_call_id_;
  /** Messaging came up at this time (0: not seen). */
  int64_t ready_at_ms_ = 0;
};

} // namespace pbr
