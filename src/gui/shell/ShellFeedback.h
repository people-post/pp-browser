#pragma once

#include "lib/ui/ShellTypes.h"
#include "gui/shell/ShellFeedbackPorts.h"

namespace pbr {

struct ShellFeedback {
  /** App fills chrome sync for dialog open/close. Clear via BindChromePorts({}). */
  static void BindChromePorts(ShellFeedbackChromePorts ports);

  static void ShowToast(ShellState& state, const std::string& message, ToastDuration duration = ToastDuration::Short,
                        float now_ms = 0.f);
  static void ExpireToasts(ShellState& state, float now_ms);
  /** Seconds until the soonest toast expiry, or < 0 if none. Used to arm power-save wake. */
  static double SecondsUntilNextFeedbackExpiry(const ShellState& state, float now_ms);
  /** Long enough to read one sentence, then gone; nobody wants to hunt for the × (Kenneth 2026-10-19). */
  static constexpr float kBannerTtlMs = 5000.f;
  /** `now_ms` ≤ 0 means the wall clock (NowMs), like ShowToast. Showing again restarts the clock. */
  static void ShowBanner(ShellState& state, const std::string& message, float now_ms = 0.f);
  /** Stays until dismissed: for things the user must act on (update available, AI not set up, call diagnostics). */
  static void ShowStickyBanner(ShellState& state, const std::string& message);
  static void DismissBanner(ShellState& state);
  /** True when the banner just expired (caller refreshes the chrome). */
  static bool ExpireBanner(ShellState& state, float now_ms);
  /** Steady-clock milliseconds: the one clock toasts, banners and ShellHost::Update share. */
  static float NowMs();
  static void ShowAlert(ShellState& state, const std::string& title, const std::string& message,
                        std::function<void()> on_ok = {}, const std::string& ok_label = {});
  static void ShowConfirm(ShellState& state, const std::string& title, const std::string& message,
                          std::function<void(bool)> on_result, const std::string& ok_label = {});
  static void ShowConfirmWithCheckbox(ShellState& state, const std::string& title, const std::string& message,
                                      const std::string& checkbox_label, bool checkbox_default,
                                      std::function<void(bool confirmed, bool checkbox_checked)> on_result);
  static void ShowPrompt(ShellState& state, const std::string& title, const std::string& message,
                         const std::string& default_value, std::function<void(bool confirmed, std::string value)> on_result);
  static void DialogOk(ShellState& state);
  static void DialogCancel(ShellState& state);

private:
  static void SyncDialogChrome(const char* reason);
  static ShellFeedbackChromePorts chrome_ports_;
};

} // namespace pbr
