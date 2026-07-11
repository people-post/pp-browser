#pragma once

#include "base/ui/ShellTypes.h"

#include <functional>
#include <string>
#include <vector>

namespace pbr {

/** Interactive PIN unlock/create gate — queues callers until secrets are ready. */
class PinGateController {
public:
  static PinGateController& Instance();

  /** If secrets already ready, runs done(true) immediately. Otherwise shows PIN UI. */
  void EnsureUnlocked(std::function<void(bool unlocked)> done);

  /** After shell load: if vault exists and locked, show blocking unlock. */
  void PromptUnlockIfVaultExists();

  void OnSubmit();
  /** Create mode only — unlock mode ignores cancel. */
  void OnCancel();
  void DirtyPinFields();

private:
  PinGateController() = default;

  void ShowGate(bool create_mode, std::function<void(bool)> done);
  void Finish(bool unlocked);
  void DrainQueue(bool unlocked);

  std::vector<std::function<void(bool)>> pending_;
  bool showing_ = false;
};

} // namespace pbr
