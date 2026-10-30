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
  /** Identity fork plus "Restore my account: <nickname>" (local backup found on a fresh install). */
  void ShowIdentityRestore(const std::string& nickname);
  /** Restore: ask for the PIN the account used (enter mode); `wrong_pin` shows "that PIN is not right". */
  void ShowRestorePin(bool wrong_pin);
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
  void OnIdentityRestore();
  /** "Restore with my recovery phrase": the link flow's PIN steps, then the phrase instead of a transfer code. */
  void OnIdentityPhrase();
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
  bool pending_link_default_pin_ = false;
  std::string pending_link_pin_;
};

} // namespace pbr
