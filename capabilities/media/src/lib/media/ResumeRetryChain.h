#pragma once

#include <chrono>
#include <cstdint>

namespace pbr {

/**
 * iOS: while the OS has the live session (AudioSession::IsInterrupted), try to take it back every kInterval until it
 * is back or the session ends (B217: Siri sends no "interruption ended", and the one try when the app came back ran
 * too early; nothing tried again for 6.5 minutes). Pure: AudioSession_Ios owns one, touches it only on its resume
 * queue, keeps the time and runs the steps (setActive) itself.
 *
 * - An interruption that begins starts a chain unless one runs (OnBegan); its first tick is kInterval later.
 * - A tick stops the chain once the session ended or is no longer interrupted (resumed another way), waits while the
 *   app is inactive (Siri, a call screen or Control Center over it: a cheap filter, not the safety — the OS decides
 *   who has the audio), and otherwise tries.
 * - A try's result: back (Ok) ends the chain; refused (Failed) or overtaken by a newer activation (Stale) waits for
 *   the next tick; the session ended (Gone) or resumed another way (Already) ends it.
 * - Logging: the 1st failure (and skip) of a chain is logged, then every 10th.
 */
class ResumeRetryChain {
public:
  using TimePoint = std::chrono::steady_clock::time_point;
  static constexpr std::chrono::milliseconds kInterval{1500};

  /** UIApplication's state; Unknown until seen (counts as "try"). */
  enum class AppState { Unknown, Active, Inactive, Background };
  enum class Action { Stop, Skip, Try };
  enum class TryResult { Ok, Failed, Stale, Gone, Already };
  enum class StopReason { None, Resumed, SessionEnded, ResumedElsewhere };

  struct TickDecision {
    Action action = Action::Stop;
    /** Stop: why. */
    StopReason stop = StopReason::None;
    /** Skip: this skip is one to log. */
    bool log_skip = false;
    /** Skip: skips in this chain so far. */
    int skips = 0;
  };

  struct TryOutcome {
    /** Schedule the next tick (else the chain ended: `stop` says why). */
    bool reschedule = false;
    StopReason stop = StopReason::None;
    /** Failed: this failure is one to log. */
    bool log_failure = false;
    /** Failed results in this chain so far. */
    int failures = 0;
    /** Ok: the metric's `tries` (this one included) and `waited_ms` (since the interruption began). */
    int tries = 0;
    std::chrono::milliseconds waited{0};
  };

  /**
   * An interruption began at `now`. True when this starts a chain (schedule its first tick in kInterval); false while
   * one runs (it carries on; its counts and wait start again from this interruption).
   */
  bool OnBegan(TimePoint now);
  TickDecision OnTick(bool session_active, bool interrupted, AppState app_state);
  /** The result of the try OnTick asked for. */
  TryOutcome OnTryResult(TryResult result, TimePoint now);

  bool Running() const { return running_; }
  /** Forget when the interruption began (a resume or the session's end): SinceBegan is 0 until the next OnBegan. */
  void ClearBegan() { began_ = false; }
  /** Since the latest interruption began (0 when none is recorded): `waited_ms` for a resume another way. */
  std::chrono::milliseconds SinceBegan(TimePoint now) const;

  /** The 1st, 11th, 21st … of something (counted from 1) is logged. */
  static bool ShouldLog(int count);
  static const char* Word(StopReason reason);
  static const char* Word(AppState state);

private:
  void End() { running_ = false; }

  bool running_ = false;
  bool began_ = false;
  TimePoint began_at_{};
  int tries_ = 0;
  int failures_ = 0;
  int skips_ = 0;
};

} // namespace pbr
