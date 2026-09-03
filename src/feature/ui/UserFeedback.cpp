#include "feature/ui/UserFeedback.h"

#include "foundation/error/AppError.h"
#include "feature/ui/ShellFeedback.h"
#include "common/PbrCompat.h"

namespace pbr {

ShellFeedbackPorts UserFeedback::ports_;

void UserFeedback::BindPorts(ShellFeedbackPorts ports) {
  ports_ = std::move(ports);
}

std::string UserFeedback::UserMessage(const Error& err) {
  return AppError::Display(err);
}

void UserFeedback::Ok(const std::string& message) {
  if (ports_.show_toast) {
    ports_.show_toast(message, ToastDuration::Short);
  }
}

void UserFeedback::Fail(const std::string& message) {
  if (ports_.show_toast) {
    ports_.show_toast(message, ToastDuration::Long);
  }
}

std::string UserFeedback::FailFrom(const Error& err) {
  const std::string display = UserMessage(err);
  Fail(display);
  return display;
}

void UserFeedback::NeedsSetup(const std::string& message) {
  if (ports_.show_banner) {
    ports_.show_banner(message);
  }
}

void UserFeedback::Alert(const std::string& title, const std::string& message, std::function<void()> on_ok,
                         const std::string& ok_label) {
  if (ports_.show_alert) {
    ports_.show_alert(title, message, std::move(on_ok), ok_label);
  }
}

void UserFeedback::Confirm(const std::string& title, const std::string& message,
                           std::function<void(bool)> on_result, const std::string& ok_label) {
  if (ports_.show_confirm) {
    ports_.show_confirm(title, message, std::move(on_result), ok_label);
  }
}

} // namespace pbr
