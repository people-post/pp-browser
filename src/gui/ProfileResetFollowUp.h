#pragma once

namespace pbr {

/** What the app does once Reset my profile has finished. */
enum class ResetFollowUp {
  None,           // the reset failed: nothing follows (the error is shown)
  RestartProcess, // desktop: the app quits in order and starts itself again on the (empty) profile
  ClearInPlace,   // phones, and a desktop that cannot restart: the first-run page over a cleared shell
};

/**
 * `restart_supported`: the OS build can start itself again (os::RestartSupported). `plan_ready`: the program file and
 * the start arguments are known (os::RestartPlan::Ready), checked before the app is told to quit, because there is
 * no way back into the running app after its shutdown.
 */
inline ResetFollowUp ChooseResetFollowUp(const bool reset_succeeded, const bool restart_supported,
                                         const bool plan_ready) {
  if (!reset_succeeded) {
    return ResetFollowUp::None;
  }
  return restart_supported && plan_ready ? ResetFollowUp::RestartProcess : ResetFollowUp::ClearInPlace;
}

} // namespace pbr
