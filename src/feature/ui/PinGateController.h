#pragma once

#include "base/ui/ShellTypes.h"

#include <functional>
#include <string>
#include <vector>

namespace pbr {

class MessagingHub;

/** Interactive PIN unlock/create gate — queues callers until secrets are ready. */
class PinGateController {
public:
  static PinGateController& Instance();

  void BindMessaging(MessagingHub& messaging);
  MessagingHub& Hub();
  const MessagingHub& Hub() const;

  /** If secrets already ready, runs done(true) immediately. Otherwise shows PIN UI. */
  void EnsureUnlocked(std::function<void(bool unlocked)> done);

  /**
   * After first present: async-scheduled silent default unlock or PIN UI when vault exists.
   * Queues EnsureUnlocked callers while unlock_in_progress.
   */
  void BeginDeferredUnlockAfterFirstPresent();

  /** True while deferred silent unlock is running (features must stay gated). */
  bool IsUnlockInProgress() const { return unlock_in_progress_; }

  void OnSubmit();
  /** Create / chooser modes only — unlock mode ignores cancel. */
  void OnCancel();
  void OnSetPin();
  void OnUseDefaultPin();
  void DirtyPinFields();

private:
  PinGateController() = default;

  void ShowChooser(std::function<void(bool)> done);
  void ShowGate(bool create_mode, std::function<void(bool)> done);
  bool TrySilentDefaultUnlock();
  void SetPinIsDefault(bool is_default);
  void Finish(bool unlocked);
  void DrainQueue(bool unlocked);

  std::vector<std::function<void(bool)>> pending_;
  bool showing_ = false;
  bool unlock_in_progress_ = false;
  bool deferred_unlock_started_ = false;
  MessagingHub* messaging_ = nullptr;

};

} // namespace pbr
