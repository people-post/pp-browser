#pragma once

#include "feature/ui/ShellPinGatePorts.h"
#include "feature/ui/UnlockGateCompletePorts.h"

#include <string>

namespace pbr {

/**
 * Shell PIN overlay — presentation only.
 * Policy / queue live in ProfileUnlockGate; Application binds UI ports from this controller.
 */
class PinGateController {
public:
  PinGateController() = default;

  void BindGateComplete(UnlockGateCompletePorts ports);
  /** Shell PIN chrome without ShellHost::Instance(). Clear via BindShellPinGate({}). */
  void BindShellPinGate(ShellPinGatePorts ports);

  /** Ports filled onto ProfileUnlockPorts::ui by Application. */
  void ShowChooser();
  void ShowUnlock();
  void Dismiss();
  void SetUnlockInProgressUi(bool in_progress);
  void ShowError(const std::string& message);

  void OnSubmit();
  /** Create / chooser modes only — unlock mode ignores cancel. */
  void OnCancel();
  void OnSetPin();
  void OnUseDefaultPin();
  void DirtyPinFields();

private:
  void ShowCreate();

  UnlockGateCompletePorts gate_complete_;
  ShellPinGatePorts shell_pin_gate_;
};

} // namespace pbr
