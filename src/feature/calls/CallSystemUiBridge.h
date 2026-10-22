#pragma once

#include "domain/calls/CallScreen.h"
#include "foundation/platform/CallSystemUi.h"

#include <cstdint>
#include <functional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Keeps the phone's own call screen (ICallSystemUi: CallKit) in step with CallPresenter's screen,
 * and turns what the user does there into the presenter's buttons (P013).
 *
 * - A ring goes to the system screen when the app is not in the foreground, or when a call push
 *   already put "Incoming call" there (the push's call is filled in with the ring's caller).
 * - Answered there, the call stays there until it ends; answered in the app, the system screen lets
 *   it go (the app's own call screen shows it).
 * - Answered or declined on the system screen before the ring was known (the push's call; the app
 *   was still waking, or locked): done to the ring once it shows, if within kPendingChoiceMs.
 *
 * UI thread. Holds no GUI: CallController wires it to the presenter.
 */
class CallSystemUiBridge {
public:
  struct Actions {
    std::function<void(bool voice_only)> accept;
    std::function<void()> decline;
    std::function<void()> leave;
    std::function<void()> toggle_mute;
    std::function<std::string(const std::string& identity)> display_name;
    /** Unix ms (tests set it). */
    std::function<int64_t()> now_ms;
  };
  static constexpr int64_t kPendingChoiceMs = 60'000;

  CallSystemUiBridge(ICallSystemUi& ui, Actions actions);
  ~CallSystemUiBridge();
  CallSystemUiBridge(const CallSystemUiBridge&) = delete;
  CallSystemUiBridge& operator=(const CallSystemUiBridge&) = delete;

  /** After every change of the presenter's screen. */
  void Sync(const CallScreen& screen, const std::string& active_call_id, bool app_foreground);
  /** The system screen rings `call_id` (its own ringtone): the app's stays quiet. */
  bool SystemRings(const std::string& call_id) const;
  /** A ring showing now goes to the system screen (so the app posts no "Incoming call" banner). */
  bool TakesRing(bool app_foreground) const;

  // ICallSystemUi::Callbacks (public for tests).
  void OnAnswer(const std::string& call_id);
  void OnEnd(const std::string& call_id);
  void OnSetMuted(const std::string& call_id, bool muted);

private:
  int64_t Now() const;
  bool Ringing(const std::string& call_id) const;

  ICallSystemUi& ui_;
  Actions actions_;
  CallScreen screen_;
  std::string active_call_id_;
  /** The call on the system screen; empty: none. */
  std::string shown_;
  /** Answered on the system screen (it keeps the call until it ends). */
  bool shown_answered_ = false;
  /** The push's call was answered / declined before its ring showed (0: no such choice). */
  int64_t answer_pending_at_ms_ = 0;
  int64_t decline_pending_at_ms_ = 0;
};

} // namespace pbr
