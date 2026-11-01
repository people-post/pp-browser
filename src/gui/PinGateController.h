#pragma once

#include "lib/ui/ShellTypes.h"
#include "gui/shell/ShellPinGatePorts.h"
#include "gui/UnlockGateCompletePorts.h"

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
  /** Shell PIN chrome. Clear via BindShellPinGate({}). */
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
  /** "Restore with my recovery phrase": the link flow's PIN steps, then the phrase instead of a transfer code. */
  void OnIdentityPhrase();
  /** The phrase page: pick a saved copy of the account (M023) to restore from too. */
  void OnUseSavedCopy();
  void OnForgotToggle();
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
  /** Within the link flow: the paste page takes a recovery phrase (M023), not a transfer code. */
  bool phrase_flow_ = false;
  /** The saved copy picked on the phrase page (exported locker file's text); empty: none. */
  std::string saved_copy_;
  bool pending_link_default_pin_ = false;
  std::string pending_link_pin_;
};

} // namespace pbr
