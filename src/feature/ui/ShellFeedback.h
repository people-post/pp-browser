#pragma once

#include "base/ui/ShellTypes.h"

namespace pbr {

struct ShellFeedback {
  static void ShowToast(ShellState& state, const std::string& message, ToastDuration duration = ToastDuration::Short,
                        float now_ms = 0.f);
  static void ExpireToasts(ShellState& state, float now_ms);
  static void ShowBanner(ShellState& state, const std::string& message);
  static void DismissBanner(ShellState& state);
  static void ShowAlert(ShellState& state, const std::string& title, const std::string& message,
                        std::function<void()> on_ok = {});
  static void ShowConfirm(ShellState& state, const std::string& title, const std::string& message,
                          std::function<void(bool)> on_result);
  static void ShowConfirmWithCheckbox(ShellState& state, const std::string& title, const std::string& message,
                                      const std::string& checkbox_label, bool checkbox_default,
                                      std::function<void(bool confirmed, bool checkbox_checked)> on_result);
  static void ShowPrompt(ShellState& state, const std::string& title, const std::string& message,
                         const std::string& default_value, std::function<void(bool confirmed, std::string value)> on_result);
  static void DialogOk(ShellState& state);
  static void DialogCancel(ShellState& state);
};

} // namespace pbr
