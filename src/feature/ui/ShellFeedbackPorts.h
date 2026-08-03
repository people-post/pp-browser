#pragma once

#include "base/ui/ShellTypes.h"

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
  std::function<void(const std::string& title, const std::string& message, std::function<void()> on_ok)> show_alert;
  std::function<void(const std::string& title, const std::string& message, std::function<void(bool)> on_result)>
      show_confirm;
  std::function<void(const std::string& title, const std::string& message, const std::string& checkbox_label,
                     bool checkbox_default, std::function<void(bool confirmed, bool checkbox_checked)> on_result)>
      show_confirm_with_checkbox;
};

/** Low-level chrome sync for ShellFeedback static helpers (dialog open/close). App-filled. */
struct ShellFeedbackChromePorts {
  std::function<ShellState&()> shell_state;
  std::function<void(bool restore_focus_after, const char* reason)> request_sync_layout;
  std::function<void()> dirty_window;
};

} // namespace pbr
