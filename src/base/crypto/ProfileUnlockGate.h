#pragma once

#include "base/crypto/ProfileSecretsService.h"
#include "common/Error.h"
#include "common/Module.h"

#include <functional>
#include <string>
#include <vector>

namespace pbr {

/**
 * Presentation hooks for interactive PIN unlock. Declared in base (gate consumer);
 * Application fills from feature PinGateController. Not a singleton.
 */
struct ProfileUnlockUiPorts {
  /** No vault yet — I'm new vs I already have an account. Falls back to show_chooser. */
  std::function<void()> show_identity_fork;
  /** No vault, "I'm new" — three-way PIN chooser. */
  std::function<void()> show_chooser;
  /** Existing vault, custom PIN — blocking unlock modal. */
  std::function<void()> show_unlock;
  /** Clear PIN overlay and unlock-in-progress chrome. */
  std::function<void()> dismiss;
  /** Silent unlock / preparing indicator. */
  std::function<void(bool in_progress)> set_unlock_in_progress;
  /** Report unlock/create failure while the PIN overlay is still up. */
  std::function<void(const std::string& message)> show_error;
  /** After chooser "Just continue" provisions the default PIN successfully. */
  std::function<void()> on_default_provisioned;
  /** After first-run link-device import succeeds (UI thread). */
  std::function<void()> on_link_imported;
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
  /** Empty-vault link-device: wrap shared DEK with `pin` and apply the bundle. */
  std::function<Roe<void>(const std::string& pin, const std::string& bundle_json)> import_link_device;

  /**
   * Run Argon2 unlock + EnsureMessagingReady off the UI thread.
   * work() may block; on_done is invoked on the UI thread.
   * When unset (tests), work runs synchronously on the caller thread.
   */
  std::function<void(std::function<Roe<void>()> work, std::function<void(Roe<void>)> on_done)> run_heavy;

  ProfileUnlockUiPorts ui;
};

/**
 * Profile vault unlock policy + caller queue.
 * Feature UI presents modals via ProfileUnlockUiPorts; submit/cancel call back into the gate.
 */
class ProfileUnlockGate : public Module {
public:
  ProfileUnlockGate();

  void BindSecrets(ProfileSecretsService& secrets);
  void BindPorts(ProfileUnlockPorts ports);

  /** If secrets + messaging already ready, runs done(true). Otherwise shows UI / silent unlock. */
  void EnsureUnlocked(std::function<void(bool unlocked)> done);

  /**
   * After first present: silent default unlock when pin_is_default, else unlock UI when vault exists.
   * Queues EnsureUnlocked callers while unlock_in_progress.
   * Heavy work runs via ports.run_heavy when set (keeps first paint interactive).
   */
  void BeginDeferredUnlockAfterFirstPresent();

  bool IsUnlockInProgress() const { return unlock_in_progress_; }

  /**
   * UI: unlock or create with the given PIN (async when run_heavy is set).
   * create_mode clears pin_is_default on success.
   */
  void CompleteWithPin(const std::string& pin, bool create_mode);
  /** UI: chooser "use default PIN" (async when run_heavy is set). */
  void CompleteWithDefaultPin();
  /** UI: first-run "I already have an account" after PIN + paste. */
  void CompleteLinkDevice(const std::string& pin, const std::string& bundle_json, bool set_default_pin);
  /** UI: cancel chooser / create only. */
  void Cancel();

private:
  ProfileSecretsService& Secrets();
  Roe<void> UnlockAndReady(const std::string& pin);
  void RunUnlockAndReadyAsync(std::string pin, bool set_default_pin, bool clear_default_pin);
  void RequestShowIdentityFork(std::function<void(bool)> done);
  void RequestShowChooser(std::function<void(bool)> done);
  void RequestShowUnlock(std::function<void(bool)> done);
  void SetUnlockInProgress(bool in_progress);
  void Finish(bool unlocked);
  void DrainQueue(bool unlocked);
  void ReportError(const std::string& message);

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
