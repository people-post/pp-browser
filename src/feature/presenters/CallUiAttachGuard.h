#pragma once

#include "lib/calls/CallLifecycleTypes.h"

#include <cstdint>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * DEV/PLAN-call-ui-never-detached.md: what a call's screen, the phone's "in a call" notification and its Hang up
 * follow besides the stored rows — the engine's own state, held in memory.
 *
 * "The engine has an open call": the backend is available, it names a call, and that call is calling out, joined,
 * connecting or connected — its mic may be open. Ringing / Accepting (the invite decides the screen), Idle and
 * ConnectFailed (its media already stopped) are not open calls.
 */
bool EngineHasOpenCall(bool available, const std::string& active_call_id, CallPhase phase);

/** The ongoing-call service (Android's "in a call" notification and its Hang up) is on: the screen or the engine has a call. */
bool OngoingCallWanted(bool presenter_has_call, bool engine_open);

/**
 * The guard's answer once it finds a call the screen lost: true hangs it up (the user cannot see that the mic is
 * open); false only logs and reports it. Kenneth has still to decide (plan, goal 2): this one line is the switch.
 */
inline constexpr bool kCallUiDetachedHangsUp = true;

/**
 * The last resort when the screen and the engine disagree about a call (the presenter's own rules should never let
 * that last): fed every frame after the presenter's refresh, it says when to report the lost call, when to hang it
 * up, and when to stop its media again if hanging up did not close it. Pure (the caller passes a steady clock).
 *
 * Lost = the engine has an open call and the presenter's call id is empty or another. It hangs up after
 * kHangUpTicks lost frames in a row that span kHangUpMs; a gap longer than kMaxGapMs between two frames (the app was
 * paused) adds no time and starts the count again. Once hung up, while the engine still has that call: its media is
 * stopped again every kStopRetryMs, at most kMaxStopRetries times.
 */
class CallUiAttachGuard {
public:
  static constexpr int kHangUpTicks = 3;
  static constexpr int64_t kHangUpMs = 5'000;
  static constexpr int64_t kMaxGapMs = 3'000;
  static constexpr int64_t kStopRetryMs = 2'000;
  static constexpr int kMaxStopRetries = 3;

  struct Input {
    /** EngineHasOpenCall. */
    bool engine_open = false;
    std::string engine_call_id;
    /** The presenter's ActiveCallId, after its refresh. */
    std::string ui_call_id;
    /** std::chrono::steady_clock, in ms. */
    int64_t steady_ms = 0;
  };

  struct Step {
    /** The first lost frame of `call_id` (once per call): report call.ui_detached. */
    bool report_detached = false;
    /** Hang `call_id` up (once per call). */
    bool hang_up = false;
    /** `call_id` was hung up and the engine still has it open: stop its media again (report stop_failed). */
    bool stop_again = false;
    std::string call_id;
  };

  Step Feed(const Input& in);

private:
  void ResetLost();

  /** The call counted, its lost frames in a row, the time they span, and the last of them. */
  std::string lost_call_id_;
  int lost_ticks_ = 0;
  int64_t lost_ms_ = 0;
  int64_t last_lost_ms_ = 0;
  /** The call call.ui_detached was reported for (once per call). */
  std::string reported_call_id_;
  /** The call hung up, when its media was last stopped, and how many times it was stopped again. */
  std::string hung_up_call_id_;
  int64_t last_stop_ms_ = 0;
  int stop_retries_ = 0;
};

} // namespace pbr
