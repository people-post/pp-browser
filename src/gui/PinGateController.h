#pragma once

#include "lib/ui/ShellTypes.h"
#include "gui/shell/ShellPinGatePorts.h"
#include "gui/PinPromptPorts.h"
#include "gui/UnlockGateCompletePorts.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace pbr {

/** What the gate needs to pick the number pad (DEV/PLAN-pin-gate-redesign.md B). Clear via BindKeypad({}). */
struct PinGateKeypadPorts {
  /** Phones (iOS / Android): the app's number pad. Unset or false (desktop): always the field. */
  std::function<bool()> keypad_available;
  /** This profile's `pin_digits`: nullopt unknown, 0 a text PIN, 4–8 a digit PIN. */
  std::function<std::optional<int>()> pin_digits;
  /**
   * Face / fingerprint unlock on (at-rest A014): `face`, `touch` or `fingerprint` (the device's sensor); "" (or
   * unset) when off or not supported — the unlock door then has no face / fingerprint key.
   */
  std::function<std::string()> biometric_kind;
};

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
  void BindKeypad(PinGateKeypadPorts ports);

  /** Ports filled onto ProfileUnlockPorts::ui by Application. */
  void ShowIdentityFork();
  void ShowChooser();
  void ShowUnlock();
  void Dismiss();
  void SetUnlockInProgressUi(bool in_progress);
  /** The gate did not start the submitted PIN (another unlock was running): the pad is usable again. */
  void OnSubmitDeclined();
  void ShowError(const std::string& message);
  /** A call is waiting for the unlock (or no longer is): the unlock screen's words follow. */
  void RefreshCallHint();
  /**
   * PIN grace window: a call waits for the locked phone's unlock (true) — "Unlock your phone to connect the call"
   * and an "Enter PIN instead" button instead of the PIN box — or no longer does (false): the unlock screen as usual.
   */
  void SetWaitingDeviceUnlock(bool waiting);
  /** Face / fingerprint unlock was lost (at-rest A014): the door's key goes, `hint` shows on it. */
  void ShowBiometricReset(const std::string& hint);

  /**
   * Settings asks for the PIN once (B2): `check` runs on what is typed; ok → the gate closes, then `on_ok`. Cancel
   * closes it without `on_ok`.
   */
  void ShowVerify(PinVerifyPurpose purpose, PinGateCheck check, std::function<void()> on_ok);
  /**
   * Settings changes the PIN (B2): the current PIN (`check_old`), the new one, again, then `change`. A known
   * `current_pin` (the app default PIN, public anyway) skips the first step.
   */
  void ShowChange(PinGateCheck check_old, PinGateChange change, std::optional<SecretText> current_pin = std::nullopt);

  void OnSubmit();
  /** Number pad: a digit 0–9 (a full PIN submits itself), backspace, and a new PIN's "OK". */
  void OnKey(int digit);
  void OnBackspace();
  void OnKeypadOk();
  /** The pad → the field ("Use the keyboard"), and back ("Use the number pad"). */
  void OnUseText();
  void OnUseKeypad();
  /** The face / fingerprint key (pad) or link (field) on the unlock door: the system prompt again. */
  void OnBiometric();
  /** "Enter PIN" on a biometric-first unlock door: the pad / field come up. */
  void OnUsePin();
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
  /** "Enter PIN instead" on the waiting screen. */
  void OnUsePinInstead();
  void DirtyPinFields();

private:
  void ShowCreate();
  void ShowLinkChooser();
  void ShowLinkPaste();
  void PullBoundPinFields();
  void ApplyPinGate();
  void Remount();
  std::string UnlockMessage() const;
  bool KeypadAvailable() const;
  std::optional<int> KnownDigits() const;
  /** The fields of a known-PIN step (unlock, verify, change's first): the pad when the PIN is known to be digits. */
  void SetupKnownPinEntry();
  /** The fields of a new-PIN step (create, change's second): the pad on phones. */
  void SetupNewPinEntry();
  /** The message of a new-PIN step in the current input mode. */
  std::string NewPinMessage() const;
  /** create, or change past its first step. */
  bool InNewPinStep() const;
  /** The dots and the OK key, from what is typed. */
  void RefreshKeypad();
  /** A known PIN is complete (pad full, or Continue): unlock / check it. */
  void SubmitKnownPin(const std::string& pin);
  /** A new PIN and its repeat are in: compare, then create / change. */
  void SubmitNewPin(const std::string& pin, const std::string& confirm);
  /** A verify / change check came back. */
  void OnCheckDone(uint64_t request, Roe<void> result, std::function<void()> then);
  /** Drops a verify / change in progress (its late result does nothing). */
  void ResetSettingsFlows();

  UnlockGateCompletePorts gate_complete_;
  ShellPinGatePorts shell_pin_gate_;
  PinGateKeypadPorts keypad_ports_;
  PinGateState pin_state_;
  bool link_flow_ = false;
  /** Within the link flow: the paste page takes a recovery phrase (M023), not a transfer code. */
  bool phrase_flow_ = false;
  /** The saved copy picked on the phrase page (exported locker file's text); empty: none. */
  std::string saved_copy_;
  bool pending_link_default_pin_ = false;
  std::string pending_link_pin_;
  /** A new PIN on the pad: 0 = its first entry (`pin`), 1 = the repeat (`pin_confirm`). */
  int new_pin_step_ = 0;
  /** Change: 0 = the current PIN, 1 = the new one. */
  int change_step_ = 0;
  SecretText change_old_pin_;
  PinVerifyPurpose verify_purpose_ = PinVerifyPurpose::ExportTransferCode;
  PinGateCheck verify_check_;
  std::function<void()> verify_on_ok_;
  PinGateCheck change_check_old_;
  PinGateChange change_submit_;
  /** Bumped on every verify / change check and on cancel: a late result only acts if it is still the current one. */
  uint64_t request_ = 0;
};

} // namespace pbr
