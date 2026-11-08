#pragma once

#include "common/Secret.h"

#include "foundation/crypto/ProfileSecretsEngine.h"
#include "common/Error.h"
#include "common/Module.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

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
  /**
   * PIN grace window, the entry not readable while the phone is locked and a call waits for the unlock: true → the
   * unlock screen says "Unlock your phone to connect the call" with an "Enter PIN instead" button; false → the
   * plain unlock screen again.
   */
  std::function<void(bool waiting)> show_waiting_device_unlock;
  /**
   * Face / fingerprint unlock is no longer set up (the entry or `biometric.bin` gone, at-rest A014): the door's
   * face / fingerprint key goes and `hint` (pin.biometric_reset_hint) shows on it.
   */
  std::function<void(const std::string& hint)> show_biometric_reset;
  /**
   * A PIN / import submitted while another unlock or a face / fingerprint prompt runs was not started: the door's pad
   * takes keys again (the shell's unlock indicator is left as it is).
   */
  std::function<void()> submit_declined;
};

/**
 * App-filled ports: messaging readiness + prefs + UI. Clear via BindPorts({}).
 * Gate does not depend on ConversationsHub or ShellHost.
 */
struct ProfileUnlockPorts {
  std::function<bool()> messaging_initialized;
  std::function<bool()> messaging_ready;
  std::function<Roe<void>()> ensure_messaging_ready;

  std::function<bool()> pin_is_default;
  std::function<void(bool is_default)> set_pin_is_default;
  /** Empty-vault link-device: wrap shared DEK with `pin` and apply the bundle. */
  std::function<Roe<void>(const SecretText& pin, const std::string& bundle_json)> import_link_device;
  /** Fresh install: the account back from its recovery phrase (M023) under `pin` for this device. */
  /** `saved_copies`: saved copies of the account the user picked (exported locker files' text). */
  std::function<Roe<void>(const SecretText& pin, const SecretText& phrase,
                          const std::vector<std::string>& saved_copies)>
      restore_recovery_phrase;

  // --- PIN grace window (#21 phase 1, DEV/PLAN-pin-grace.md). Without try_grace_unlock there is no grace step. ---
  /** The window the user chose, in minutes; 0 = off (the PIN is asked at every cold start). */
  std::function<int()> grace_minutes;
  /** ProfileSecretsEngine::TryGraceUnlock: Found means the vault is open with the remembered DEK. */
  std::function<Roe<GraceUnlock::Result>()> try_grace_unlock;
  /** ProfileSecretsEngine::RememberGrace after a PIN unlock; a failure is reported, never fatal. */
  std::function<Roe<void>(int minutes)> remember_grace;
  /**
   * A PIN the user chose opened the vault (typed on the gate, set on it, or the one an account was brought here
   * under): called once per unlock, on the UI thread, with that PIN — never for the app's default PIN. The app keeps
   * the PIN's kind (`pin_digits`, DEV/PLAN-pin-gate-redesign.md B) from it; the gate cannot read preferences.
   */
  std::function<void(const SecretText& pin)> on_pin_verified;
  // --- Face / fingerprint unlock (at-rest A014, DEV/PLAN-pin-gate-redesign.md C). Without try_biometric_unlock or
  // biometric_enabled there is no biometric step. Order: default PIN → grace window → biometric → dev PIN → the door.
  /** The switch is on for this profile (ProfileSecretsEngine::BiometricEnabled). */
  std::function<bool()> biometric_enabled;
  /**
   * ProfileSecretsEngine::TryBiometricUnlock: shows the system prompt with `prompt` and blocks until it is answered
   * (run through run_heavy, never on the UI thread); Found means the vault is open.
   */
  std::function<Roe<BiometricUnlock::Result>(const std::string& prompt)> try_biometric_unlock;
  /** The words under the system prompt (unset: none). */
  std::function<std::string()> biometric_prompt_text;
  /** os::AppShownNow: the app is on screen (unset: it is). A door raised off screen prompts on the next foreground. */
  std::function<bool()> app_shown;
  /** The biometric pieces were found gone (Missing; the engine deleted them): the settings switch reads off. */
  std::function<void()> on_biometric_lost;
  /** A call answered on the phone's call screen waits for the unlock (ICallSystemUi::HasPendingPushCall). */
  std::function<bool()> has_pending_push_call;
  /** os::ProtectedDataAvailable. */
  std::function<bool()> protected_data_available;
  /** os::OnProtectedDataAvailable (the handler runs on any thread) / os::RemoveProtectedDataHandler. */
  std::function<uint64_t(std::function<void()>)> on_protected_data_available;
  std::function<void(uint64_t)> remove_protected_data_handler;
  /** Post to the UI thread (unset: run inline). */
  std::function<void(std::function<void()>)> run_on_ui;
  /** Run on the UI thread after `delay` (unset: the call waits without a time limit). */
  std::function<void(std::chrono::milliseconds delay, std::function<void()>)> run_later;

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

  void BindSecrets(ProfileSecretsEngine& secrets);
  void BindPorts(ProfileUnlockPorts ports);

  /** The vault refused the PIN (its one code covers a wrong PIN and a damaged vault). */
  static bool IsWrongPinError(const Error& error);

  /** If secrets + messaging already ready, runs done(true). Otherwise shows UI / silent unlock. */
  void EnsureUnlocked(std::function<void(bool unlocked)> done);

  /**
   * After first present: silent default unlock when pin_is_default, else the PIN grace window (an entry still
   * open unlocks without a PIN), else the dev PIN (local builds) or the unlock UI when a vault exists, else
   * (fresh install) the identity fork.
   * Queues EnsureUnlocked callers while unlock_in_progress.
   * Heavy work runs via ports.run_heavy when set (keeps first paint interactive).
   */
  void BeginDeferredUnlockAfterFirstPresent();

  bool IsUnlockInProgress() const { return unlock_in_progress_; }

  /**
   * UI: unlock or create with the given PIN (async when run_heavy is set).
   * create_mode clears pin_is_default on success.
   */
  void CompleteWithPin(const SecretText& pin, bool create_mode);
  /** UI: chooser "use default PIN" (async when run_heavy is set). */
  void CompleteWithDefaultPin();
  /** UI: first-run "I already have an account" after PIN + paste. */
  void CompleteLinkDevice(const SecretText& pin, const std::string& bundle_json, bool set_default_pin);
  /** "Restore with my recovery phrase": restore under `pin`, then unlock — no first-run wizard, as after a link. */
  void CompletePhraseRestore(const SecretText& pin, const SecretText& phrase, bool set_default_pin,
                             std::vector<std::string> saved_copies = {});
  /** UI: cancel chooser / create / paste. Without a vault this returns to the identity fork. */
  void Cancel();

  /** UI: "Enter PIN instead" while waiting for the phone's unlock: the plain unlock screen, at once. */
  void UseDevicePinInstead();
  /**
   * The app came to the foreground: a grace entry the locked phone kept unreadable is tried again; otherwise a door
   * still up whose face / fingerprint prompt has not run yet (raised off screen) prompts once.
   */
  void OnForeground();
  /** UI: the door's face / fingerprint key (or link): prompts again, unless a prompt is still running. */
  void TryBiometric();

  /** How long a call answered on a locked phone waits for its unlock (Leo's pending-call limit). */
  static constexpr std::chrono::minutes kDeviceUnlockWait{3};

private:
  /** Which grace attempt: the first, the re-read right after the handler is registered, or a later retry. */
  enum class GraceStep { First, Recheck, Retry };
  void RunGraceAttempt(GraceStep step);
  void OnGraceAttemptDone(GraceStep step, std::optional<GraceUnlock::Status> status, Roe<void> result);
  void OnGraceUnavailable(GraceStep step);
  void RetryGrace();
  void EndDeviceUnlockWait();
  void StopGrace();
  void SetGraceAttemptInProgress(bool in_progress);
  void RememberGraceAfterPinUnlock();
  /** The automatic face / fingerprint prompt may run now (switch on, on screen, not yet prompted at this door…). */
  bool BiometricDue() const;
  /** The switch on and nothing in the way of a prompt (no unlock running, no prompt in flight, vault still locked). */
  bool BiometricPossible() const;
  void StartBiometric();
  void OnBiometricDone(uint64_t door, std::optional<BiometricUnlock::Result> got, Roe<void> result);
  /** The dev PIN (local builds) or the unlock screen: the deferred unlock without the grace window. */
  void UnlockWithoutGrace();
  /** A first secrets use that brings an existing account (link / phrase): `import` under `pin`, then unlock. */
  void CompleteAccountImport(const SecretText& pin, bool set_default_pin, std::function<Roe<void>()> import,
                             const char* mark);
  ProfileSecretsEngine& Secrets();
  Roe<void> UnlockAndReady(const SecretText& pin);
  /** `remember_grace`: a PIN the user chose opened the vault (not the default PIN) — keep the grace window. */
  void RunUnlockAndReadyAsync(SecretText pin, bool set_default_pin, bool clear_default_pin, bool remember_grace);
  void RequestShowIdentityFork(std::function<void(bool)> done);
  void RequestShowChooser(std::function<void(bool)> done);
  void RequestShowUnlock(std::function<void(bool)> done);
  void SetUnlockInProgress(bool in_progress);
  /** A PIN / import submitted while another unlock or a prompt runs: not started, and the door told so. */
  void DeclineSubmit();
  void Finish(bool unlocked);
  void DrainQueue(bool unlocked);
  void ReportError(const std::string& message);
  void ReportError(const Error& error);

  ProfileSecretsEngine* secrets_ = nullptr;
  ProfileUnlockPorts ports_;
  std::vector<std::function<void(bool)>> pending_;
  bool showing_ = false;
  bool unlock_in_progress_ = false;
  /**
   * The door's timer for unlock.done ms=: set at its FIRST unlock.start (a wrong PIN and the retry keep it), or at a
   * grace attempt's start (cleared again when that attempt does not unlock, so the PIN after it times itself);
   * cleared when the door closes (Finish). ms=0 only when neither was seen.
   */
  std::optional<std::chrono::steady_clock::time_point> unlock_started_;
  /** unlock_started_ came from a grace attempt (not a PIN / silent unlock). */
  bool unlock_started_by_grace_ = false;
  bool deferred_unlock_started_ = false;
  /** The grace entry was Unavailable: retried when protected data comes back or the app comes forward. */
  bool grace_pending_ = false;
  /** The unlock screen says "unlock your phone to connect the call". */
  bool waiting_device_unlock_ = false;
  /** Bumped when a wait ends, so a stale timeout does nothing. */
  uint64_t device_unlock_wait_generation_ = 0;
  std::optional<uint64_t> protected_data_handler_;
  /** The unlock door (show_unlock) is up: from its show until the gate finishes. */
  bool unlock_door_up_ = false;
  /** The automatic prompt ran at this door (cleared when the door rises and when it closes). */
  bool biometric_prompted_this_door_ = false;
  bool biometric_in_flight_ = false;
  /** An unlock finished while the prompt was in flight: its late Found does not lock the vault. */
  bool unlocked_beside_prompt_ = false;
  /** Bumped when the door rises and when it closes: a prompt's answer for another door is dropped. */
  uint64_t door_generation_ = 0;
};

/** Shared bootstrap / CLI path: Unlock then ensure_messaging_ready. */
Roe<void> UnlockProfileSecretsAndReady(ProfileSecretsEngine& secrets, const SecretText& pin,
                                       const std::function<Roe<void>()>& ensure_messaging_ready);

} // namespace pbr
