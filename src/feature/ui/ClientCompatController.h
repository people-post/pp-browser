#pragma once

#include "base/net/ClientCompat.h"
#include "common/Module.h"
#include "feature/messaging/MessagingCompatPorts.h"
#include "feature/ui/ShellFeedbackPorts.h"

#include <string>

namespace pbr {

/** Fetches relay client-compat and surfaces update-required / update-available UX. */
class ClientCompatController : public Module {
public:
  ClientCompatController();

  void BindCompatPorts(MessagingCompatPorts ports);
  /** Dialog / banner feedback without ShellHost::Instance(). Clear via BindShellFeedback({}). */
  void BindShellFeedback(ShellFeedbackPorts ports);

  /** Non-blocking: IO fetch/cache then UI gate/banner. Safe after MessagingHub init. */
  void CheckAsync();

  /** Apply a resolved document on the UI thread (also used by tests). */
  void ApplyDocument(const ClientCompatDocument& doc);

  CompatUiAction LastAction() const { return last_action_; }

private:
  void PresentAction(CompatUiAction action, const ClientCompatDocument& doc);
  void ShowUpdateRequired(const ClientCompatDocument& doc);

  CompatUiAction last_action_ = CompatUiAction::None;
  bool soft_banner_shown_ = false;
  bool force_dialog_shown_ = false;
  std::string upgrade_url_;
  MessagingCompatPorts compat_ports_;
  ShellFeedbackPorts shell_feedback_;
};

} // namespace pbr
