#pragma once

#include "base/crypto/ProfileUnlockGate.h"

#include <string>

namespace pbr {

/**
 * Shell PIN overlay — presentation only.
 * Policy / queue live in ProfileUnlockGate; Application binds UI ports from this controller.
 */
class PinGateController {
public:
  PinGateController() = default;

  void BindGate(ProfileUnlockGate& gate);

  /** Ports filled onto ProfileUnlockPorts::ui by Application. */
  void ShowChooser();
  void ShowUnlock();
  void Dismiss();
  void SetUnlockInProgressUi(bool in_progress);

  void OnSubmit();
  /** Create / chooser modes only — unlock mode ignores cancel. */
  void OnCancel();
  void OnSetPin();
  void OnUseDefaultPin();
  void DirtyPinFields();

private:
  void ShowCreate();

  ProfileUnlockGate* gate_ = nullptr;
};

} // namespace pbr
