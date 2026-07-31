#pragma once

#include "base/crypto/ProfileSecretsService.h"
#include "common/Error.h"

#include <functional>
#include <string>
#include <vector>

namespace pbr {

/**
 * Presentation hooks for interactive PIN unlock. Declared in base (gate consumer);
 * Application fills from feature PinGateController. Not a singleton.
 */
struct ProfileUnlockUiPorts {
  /** No vault yet — three-way chooser. */
  std::function<void()> show_chooser;
  /** Existing vault, custom PIN — blocking unlock modal. */
  std::function<void()> show_unlock;
  /** Clear PIN overlay and unlock-in-progress chrome. */
  std::function<void()> dismiss;
  /** Silent unlock / preparing indicator. */
  std::function<void(bool in_progress)> set_unlock_in_progress;
};

/**
 * App-filled ports: messaging readiness + prefs + UI. Clear via BindPorts({}).
 * Gate does not depend on MessagingHub or ShellHost.
 */
struct ProfileUnlockPorts {
  std::function<bool()> messaging_initialized;
  std::function<bool()> messaging_ready;
  std::function<Roe<void>()> ensure_messaging_ready;

  std::function<bool()> pin_is_default;
  std::function<void(bool is_default)> set_pin_is_default;

  ProfileUnlockUiPorts ui;
};

/**
 * Profile vault unlock policy + caller queue.
 * Feature UI presents modals via ProfileUnlockUiPorts; submit/cancel call back into the gate.
 */
class ProfileUnlockGate {
public:
  ProfileUnlockGate() = default;

  void BindSecrets(ProfileSecretsService& secrets);
  void BindPorts(ProfileUnlockPorts ports);

  /** If secrets + messaging already ready, runs done(true). Otherwise shows UI / silent unlock. */
  void EnsureUnlocked(std::function<void(bool unlocked)> done);

  /**
   * After first present: silent default unlock when pin_is_default, else unlock UI when vault exists.
   * Queues EnsureUnlocked callers while unlock_in_progress.
   */
  void BeginDeferredUnlockAfterFirstPresent();

  bool IsUnlockInProgress() const { return unlock_in_progress_; }

  /**
   * UI: unlock or create with the given PIN.
   * create_mode clears pin_is_default on success.
   */
  Roe<void> CompleteWithPin(const std::string& pin, bool create_mode);
  /** UI: chooser "use default PIN". */
  Roe<void> CompleteWithDefaultPin();
  /** UI: cancel chooser / create only. */
  void Cancel();

private:
  ProfileSecretsService& Secrets();
  Roe<void> UnlockAndReady(const std::string& pin);
  bool TrySilentDefaultUnlock();
  void RequestShowChooser(std::function<void(bool)> done);
  void RequestShowUnlock(std::function<void(bool)> done);
  void SetUnlockInProgress(bool in_progress);
  void Finish(bool unlocked);
  void DrainQueue(bool unlocked);

  ProfileSecretsService* secrets_ = nullptr;
  ProfileUnlockPorts ports_;
  std::vector<std::function<void(bool)>> pending_;
  bool showing_ = false;
  bool unlock_in_progress_ = false;
  bool deferred_unlock_started_ = false;
};

/** Shared bootstrap / CLI path: Unlock then ensure_messaging_ready. */
Roe<void> UnlockProfileSecretsAndReady(ProfileSecretsService& secrets, const std::string& pin,
                                       const std::function<Roe<void>()>& ensure_messaging_ready);

} // namespace pbr
