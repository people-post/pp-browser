#pragma once

#include "domain/ui/ShellTypes.h"
#include "feature/ui/shell/ShellPinGatePorts.h"
#include "feature/ui/UnlockGateCompletePorts.h"

#include <string>

namespace pbr {

/**
 * Shell PIN overlay — presentation only.
 * Policy / queue live in ProfileUnlockGate; Application binds UI ports from this controller.
 * Owns local PinGateState and pushes apply-only copies into ShellHost.
 */
class PinGateController {
public:
  PinGateController() = default;

  void BindGateComplete(UnlockGateCompletePorts ports);
  /** Shell PIN chrome without ShellHost::Instance(). Clear via BindShellPinGate({}). */
  void BindShellPinGate(ShellPinGatePorts ports);

  /** Ports filled onto ProfileUnlockPorts::ui by Application. */
  void ShowIdentityFork();
  void ShowChooser();
  void ShowUnlock();
  void Dismiss();
  void SetUnlockInProgressUi(bool in_progress);
  void ShowError(const std::string& message);

  void OnSubmit();
  /** Create / chooser / identity-fork / link-paste may cancel. Unlock ignores cancel. */
  void OnCancel();
  void OnSetPin();
  void OnUseDefaultPin();
  void OnIdentityNew();
  void OnIdentityLink();
  void DirtyPinFields();

private:
  void ShowCreate();
  void ShowLinkChooser();
  void ShowLinkPaste();
  void PullBoundPinFields();
  void ApplyPinGate();

  UnlockGateCompletePorts gate_complete_;
  ShellPinGatePorts shell_pin_gate_;
  PinGateState pin_state_;
  bool link_flow_ = false;
  bool pending_link_default_pin_ = false;
  std::string pending_link_pin_;
};

} // namespace pbr
