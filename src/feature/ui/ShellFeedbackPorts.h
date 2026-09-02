#pragma once

#include "domain/ui/ShellTypes.h"

#include <functional>
#include <string>

namespace pbr {

/**
 * Shell feedback ports (toast, banner, dialog). Declared here (consumer);
 * Application fills from ShellHost + ShellFeedback. Not a singleton.
 */
struct ShellFeedbackPorts {
  std::function<void(const std::string& message, ToastDuration duration)> show_toast;
  std::function<void(const std::string& message)> show_banner;
  std::function<void()> dismiss_banner;
  std::function<void(const std::string& title, const std::string& message, std::function<void()> on_ok,
                     const std::string& ok_label)>
      show_alert;
  std::function<bool()> dialog_active;
  std::function<void(const std::string& title, const std::string& message, std::function<void(bool)> on_result,
                     const std::string& ok_label)>
      show_confirm;
  std::function<void(const std::string& title, const std::string& message, const std::string& checkbox_label,
                     bool checkbox_default, std::function<void(bool confirmed, bool checkbox_checked)> on_result)>
      show_confirm_with_checkbox;
  std::function<void(const std::string& title, const std::string& message, const std::string& default_value,
                     std::function<void(bool confirmed, std::string value)> on_result)>
      show_prompt;
};

class ShellHost;

/** Bind global feedback chrome + shared toast/banner/dialog ports from ShellHost. */
ShellFeedbackPorts BindSharedShellFeedback(ShellHost& shell);

/** Low-level chrome sync for ShellFeedback static helpers (dialog open/close). App-filled. */
struct ShellFeedbackChromePorts {
  std::function<ShellState&()> shell_state;
  /** Presence remount into #shell-dialog-mount (not full SyncLayout). */
  std::function<void()> remount_dialog;
  /** Binding-only toast/banner/dialog field refresh (not grab-bag DirtyWindow). */
  std::function<void()> dirty_feedback;
};

} // namespace pbr
