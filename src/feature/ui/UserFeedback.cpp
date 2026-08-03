#include "feature/ui/UserFeedback.h"

#include "base/error/AppError.h"
#include "feature/ui/ShellFeedback.h"

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

} // namespace pbr
