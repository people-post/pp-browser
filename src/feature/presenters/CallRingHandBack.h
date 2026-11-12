#pragma once

#include <cstdint>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Whether the user can see the app's own ring page now (DEV/PLAN-android-incoming-call-screen.md step 1): the app in
 * front, the screen on, the phone unlocked, and PP not behind its PIN. What the platform could not say counts as not
 * seen. Pure: the caller asks the platform each time and passes the answers in.
 */
struct CallRingUi {
  bool app_foreground = false;
  /** The platform answered the screen question (false: a query failed). */
  bool screen_known = false;
  bool screen_interactive = false;
  bool keyguard_locked = true;
  /** PP's PIN gate is up (its vault is not open). */
  bool pin_locked = false;
};

bool CallRingUiVisible(const CallRingUi& ui);

/**
 * Hand a ring the phone's own call screen rings for us (Android's call notification) back to the app's ring page:
 * only while the user can see the app, the call was not answered there, and no call push waits for its ring.
 */
struct CallRingHandBackInput {
  /** The system screen rings a call for us (it still rings on the presenter's screen). */
  bool system_rings = false;
  bool answered_on_system = false;
  bool pending_push_call = false;
  CallRingUi ui;
};

/** Everything but the time: the conditions the settle time counts. */
bool CallRingHandBackConditions(const CallRingHandBackInput& in);

/** The conditions have held this long before a hand-back (MIUI let the app go ~30 ms after raising it once). */
inline constexpr int64_t kCallRingHandBackSettleMs = 300;

/** Hand back now: the conditions hold and have held for `held_ms` ≥ kCallRingHandBackSettleMs. */
bool CallRingHandBack(const CallRingHandBackInput& in, int64_t held_ms);

} // namespace pbr
