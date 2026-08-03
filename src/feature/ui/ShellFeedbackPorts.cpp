#include "feature/ui/ShellFeedbackPorts.h"

#include "feature/ui/ShellFeedback.h"
#include "feature/ui/ShellHost.h"
#include "feature/ui/UserFeedback.h"

namespace pbr {

ShellFeedbackPorts BindSharedShellFeedback(ShellHost& shell) {
  ShellFeedbackChromePorts feedback_chrome;
  feedback_chrome.shell_state = [&shell]() -> ShellState& { return shell.State(); };
  feedback_chrome.request_sync_layout = [&shell](const bool restore, const char* reason) {
    shell.RequestSyncLayout(restore, reason);
  };
  feedback_chrome.dirty_window = [&shell]() { shell.DirtyWindow(); };
  ShellFeedback::BindChromePorts(std::move(feedback_chrome));

  ShellFeedbackPorts feedback;
  feedback.show_toast = [&shell](const std::string& message, const ToastDuration duration) {
    ShellFeedback::ShowToast(shell.State(), message, duration);
    shell.DirtyWindow();
  };
  feedback.show_banner = [&shell](const std::string& message) {
    ShellFeedback::ShowBanner(shell.State(), message);
    shell.DirtyWindow();
  };
  feedback.dismiss_banner = [&shell]() {
    ShellFeedback::DismissBanner(shell.State());
    shell.DirtyWindow();
  };
  feedback.show_alert = [&shell](const std::string& title, const std::string& message,
                                   std::function<void()> on_ok) {
    ShellFeedback::ShowAlert(shell.State(), title, message, std::move(on_ok));
  };
  feedback.show_confirm = [&shell](const std::string& title, const std::string& message,
                                     std::function<void(bool)> on_result) {
    ShellFeedback::ShowConfirm(shell.State(), title, message, std::move(on_result));
  };
  feedback.show_confirm_with_checkbox =
      [&shell](const std::string& title, const std::string& message, const std::string& checkbox_label,
               const bool checkbox_default, std::function<void(bool, bool)> on_result) {
        ShellFeedback::ShowConfirmWithCheckbox(shell.State(), title, message, checkbox_label, checkbox_default,
                                             std::move(on_result));
      };
  feedback.show_prompt = [&shell](const std::string& title, const std::string& message,
                                  const std::string& default_value,
                                  std::function<void(bool, std::string)> on_result) {
    ShellFeedback::ShowPrompt(shell.State(), title, message, default_value, std::move(on_result));
  };
  UserFeedback::BindPorts(feedback);
  return feedback;
}

} // namespace pbr
