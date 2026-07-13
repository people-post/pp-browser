#include "feature/ui/UserFeedback.h"

#include "base/error/AppError.h"
#include "feature/ui/ShellFeedback.h"
#include "feature/ui/ShellHost.h"

namespace pbr {

std::string UserFeedback::UserMessage(const Error& err) {
  return AppError::Display(err);
}

void UserFeedback::Ok(const std::string& message) {
  ShellFeedback::ShowToast(ShellHost::Instance().State(), message);
  ShellHost::Instance().DirtyWindow();
}

void UserFeedback::Fail(const std::string& message) {
  ShellFeedback::ShowToast(ShellHost::Instance().State(), message, ToastDuration::Long);
  ShellHost::Instance().DirtyWindow();
}

std::string UserFeedback::FailFrom(const Error& err) {
  const std::string display = UserMessage(err);
  Fail(display);
  return display;
}

void UserFeedback::NeedsSetup(const std::string& message) {
  ShellFeedback::ShowBanner(ShellHost::Instance().State(), message);
  ShellHost::Instance().DirtyWindow();
}

} // namespace pbr
