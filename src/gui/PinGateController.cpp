#include "gui/PinGateController.h"

#include "foundation/crypto/PinDefaults.h"
#include "foundation/crypto/PinRules.h"
#include "foundation/crypto/ProfileUnlockGate.h"
#include "foundation/i18n/LocalizationService.h"
#include "foundation/platform/CallSystemUi.h"
#include "foundation/platform/PendingNotificationTap.h"
#include "gui/UserFeedback.h"

#include <utility>

namespace pbr {

void PinGateController::BindGateComplete(UnlockGateCompletePorts ports) {
  gate_complete_ = std::move(ports);
}

void PinGateController::BindShellPinGate(ShellPinGatePorts ports) {
  shell_pin_gate_ = std::move(ports);
}

void PinGateController::BindKeypad(PinGateKeypadPorts ports) {
  keypad_ports_ = std::move(ports);
}

void PinGateController::PullBoundPinFields() {
  if (!shell_pin_gate_.pin_gate_snapshot) {
    return;
  }
  const PinGateState snap = shell_pin_gate_.pin_gate_snapshot();
  pin_state_.pin = snap.pin;
  pin_state_.pin_confirm = snap.pin_confirm;
  pin_state_.link_payload = snap.link_payload;
  pin_state_.link_payload_full = snap.link_payload_full;
  pin_state_.payload_hidden = snap.payload_hidden;
}

void PinGateController::ApplyPinGate() {
  if (shell_pin_gate_.apply_pin_gate) {
    shell_pin_gate_.apply_pin_gate(pin_state_);
  }
}

void PinGateController::Remount() {
  if (shell_pin_gate_.remount_pin_gate) {
    shell_pin_gate_.remount_pin_gate();
  }
}

bool PinGateController::KeypadAvailable() const {
  return keypad_ports_.keypad_available && keypad_ports_.keypad_available();
}

std::optional<int> PinGateController::KnownDigits() const {
  return keypad_ports_.pin_digits ? keypad_ports_.pin_digits() : std::nullopt;
}

bool PinGateController::InNewPinStep() const {
  return pin_state_.create_mode || (pin_state_.change_mode && change_step_ == 1);
}

void PinGateController::SetupKnownPinEntry() {
  const std::optional<int> digits = KnownDigits();
  const bool digit_pin = KeypadAvailable() && IsDigitPinLength(digits);
  pin_state_.keypad = digit_pin;
  pin_state_.keypad_offered = digit_pin;
  pin_state_.keypad_busy = false;
  pin_state_.keypad_slots = digit_pin ? *digits : 0;
  pin_state_.keypad_text_label = Tr("pin.keypad_use_text").c_str();
  pin_state_.ask_confirm = false;
  pin_state_.pin = "";
  pin_state_.pin_confirm = "";
  new_pin_step_ = 0;
  RefreshKeypad();
}

void PinGateController::SetupNewPinEntry() {
  pin_state_.keypad = KeypadAvailable();
  pin_state_.keypad_offered = pin_state_.keypad;
  pin_state_.keypad_busy = false;
  pin_state_.keypad_text_label = Tr("pin.create_use_text").c_str();
  pin_state_.ask_confirm = !pin_state_.keypad;
  pin_state_.pin = "";
  pin_state_.pin_confirm = "";
  new_pin_step_ = 0;
  RefreshKeypad();
}

std::string PinGateController::NewPinMessage() const {
  if (pin_state_.keypad) {
    return new_pin_step_ == 0 ? Tr("pin.create_digits_message") : Tr("pin.create_repeat_message");
  }
  if (pin_state_.change_mode) {
    return Tr("pin.change_new_message");
  }
  return link_flow_ ? Tr("pin.link_create_message") : Tr("pin.create_message");
}

void PinGateController::RefreshKeypad() {
  const int typed = static_cast<int>(std::string(pin_state_.pin.c_str()).size());
  const int repeated = static_cast<int>(std::string(pin_state_.pin_confirm.c_str()).size());
  pin_state_.keypad_ok_shown = false;
  if (!pin_state_.keypad) {
    pin_state_.keypad_filled = 0;
    return;
  }
  if (InNewPinStep()) {
    // The first entry's dots grow with it; the repeat shows as many as the first entry had.
    pin_state_.keypad_slots = typed;
    pin_state_.keypad_filled = new_pin_step_ == 0 ? typed : repeated;
    pin_state_.keypad_ok_shown = new_pin_step_ == 0 && typed >= 4;
    return;
  }
  pin_state_.keypad_filled = typed;
}

void PinGateController::ResetSettingsFlows() {
  ++request_;
  verify_check_ = {};
  verify_on_ok_ = {};
  change_check_old_ = {};
  change_submit_ = {};
  change_step_ = 0;
  change_old_pin_ = SecretText();
  new_pin_step_ = 0;
}

void PinGateController::DirtyPinFields() {
  if (shell_pin_gate_.dirty_pin_gate) {
    shell_pin_gate_.dirty_pin_gate();
  }
}

void PinGateController::SetUnlockInProgressUi(const bool in_progress) {
  if (shell_pin_gate_.set_unlock_in_progress) {
    shell_pin_gate_.set_unlock_in_progress(in_progress);
  }
  if (in_progress) {
    if (shell_pin_gate_.set_activity) {
      shell_pin_gate_.set_activity(true, Tr("startup.preparing").c_str());
    }
  } else if (shell_pin_gate_.set_activity) {
    shell_pin_gate_.set_activity(false, {});
  }
  DirtyPinFields();
}

void PinGateController::OnSubmitDeclined() {
  // The gate did not start this PIN (another unlock ran): the pad takes keys again, its full entry cleared to be typed
  // anew. Settings' verify / change keep their own busy state.
  if (!pin_state_.active || !pin_state_.keypad_busy || pin_state_.verify_mode || pin_state_.change_mode ||
      !shell_pin_gate_.apply_pin_gate) {
    return;
  }
  PullBoundPinFields();
  pin_state_.keypad_busy = false;
  if (InNewPinStep()) {
    pin_state_.pin_confirm = "";
  } else {
    pin_state_.pin = "";
  }
  RefreshKeypad();
  ApplyPinGate();
  DirtyPinFields();
}

void PinGateController::ShowError(const std::string& message) {
  if (!shell_pin_gate_.apply_pin_gate) {
    return;
  }
  if (!pin_state_.active) {
    ShowUnlock();
  }
  PullBoundPinFields();
  pin_state_.error = message.c_str();
  if (pin_state_.keypad) {
    // The pad starts over: a wrong PIN clears its dots; a new PIN that could not be set is asked for again.
    pin_state_.keypad_busy = false;
    pin_state_.pin = "";
    pin_state_.pin_confirm = "";
    if (InNewPinStep()) {
      new_pin_step_ = 0;
      pin_state_.message = NewPinMessage().c_str();
    }
    RefreshKeypad();
  }
  ApplyPinGate();
  DirtyPinFields();
}

std::string PinGateController::UnlockMessage() const {
  const bool call_waiting = ICallSystemUi::Instance().HasPendingPushCall() ||
                            PendingNotificationTap::Instance().PeekCallRoute(PendingNotificationTap::Clock::now());
  if (call_waiting) {
    return Tr("pin.unlock_call_message");
  }
  if (pin_state_.biometric_first) {
    return Tr(std::string("pin.biometric_first.") + pin_state_.biometric_kind.c_str());
  }
  return Tr("pin.unlock_message");
}

void PinGateController::RefreshCallHint() {
  if (!pin_state_.active || !pin_state_.unlock_mode || pin_state_.waiting_device_unlock ||
      !shell_pin_gate_.apply_pin_gate) {
    return;
  }
  PullBoundPinFields();
  pin_state_.message = UnlockMessage().c_str();
  ApplyPinGate();
  DirtyPinFields();
}

void PinGateController::SetWaitingDeviceUnlock(const bool waiting) {
  if (!pin_state_.active || !pin_state_.unlock_mode || !shell_pin_gate_.apply_pin_gate ||
      pin_state_.waiting_device_unlock == waiting) {
    return;
  }
  PullBoundPinFields();
  pin_state_.waiting_device_unlock = waiting;
  pin_state_.forgot_open = false;
  pin_state_.error = "";
  pin_state_.message = (waiting ? Tr("pin.unlock_phone_call_message") : UnlockMessage()).c_str();
  ApplyPinGate();
  DirtyPinFields();
}

void PinGateController::ShowBiometricReset(const std::string& hint) {
  if (!pin_state_.active || !pin_state_.unlock_mode || !shell_pin_gate_.apply_pin_gate) {
    return;
  }
  PullBoundPinFields();
  const bool was_first = pin_state_.biometric_first;
  pin_state_.biometric_key = false;
  pin_state_.biometric_first = false;
  pin_state_.biometric_hint = hint.c_str();
  pin_state_.message = UnlockMessage().c_str();
  ApplyPinGate();
  if (was_first) {
    Remount();  // the pad / field instead of the key-only door
    return;
  }
  DirtyPinFields();
}

void PinGateController::OnUsePin() {
  if (!pin_state_.active || !pin_state_.unlock_mode || !pin_state_.biometric_first ||
      pin_state_.waiting_device_unlock) {
    return;
  }
  pin_state_.biometric_first = false;
  pin_state_.message = UnlockMessage().c_str();
  ApplyPinGate();
  Remount();  // the pad / field instead of the key-only door
}

void PinGateController::OnBiometric() {
  if (!pin_state_.active || !pin_state_.unlock_mode || !pin_state_.biometric_key || pin_state_.keypad_busy ||
      pin_state_.waiting_device_unlock || !gate_complete_.try_biometric) {
    return;
  }
  gate_complete_.try_biometric();
}

void PinGateController::OnUsePinInstead() {
  if (!pin_state_.active || !pin_state_.waiting_device_unlock || !gate_complete_.use_device_pin_instead) {
    return;
  }
  gate_complete_.use_device_pin_instead();
}

void PinGateController::Dismiss() {
  ResetSettingsFlows();
  pin_state_ = {};
  link_flow_ = false;
  phrase_flow_ = false;
  pending_link_default_pin_ = false;
  pending_link_pin_.clear();
  ApplyPinGate();
  if (shell_pin_gate_.remount_pin_gate) {
    shell_pin_gate_.remount_pin_gate();
  }
}

void PinGateController::ShowIdentityFork() {
  if (!shell_pin_gate_.apply_pin_gate) {
    return;
  }
  link_flow_ = false;
  phrase_flow_ = false;
  pending_link_default_pin_ = false;
  pending_link_pin_.clear();
  ResetSettingsFlows();
  pin_state_ = {};
  pin_state_.active = true;
  pin_state_.identity_fork_mode = true;
  pin_state_.title = Tr("pin.identity_fork_title").c_str();
  pin_state_.message = Tr("pin.identity_fork_message").c_str();
  ApplyPinGate();
  if (shell_pin_gate_.remount_pin_gate) {
    shell_pin_gate_.remount_pin_gate();
  }
}

void PinGateController::ShowChooser() {
  if (!shell_pin_gate_.apply_pin_gate) {
    return;
  }
  link_flow_ = false;
  phrase_flow_ = false;
  ResetSettingsFlows();
  pin_state_ = {};
  pin_state_.active = true;
  pin_state_.chooser_mode = true;
  pin_state_.title = Tr("pin.chooser_title").c_str();
  pin_state_.message = Tr("pin.chooser_message").c_str();
  ApplyPinGate();
  if (shell_pin_gate_.remount_pin_gate) {
    shell_pin_gate_.remount_pin_gate();
  }
}

void PinGateController::ShowLinkChooser() {
  if (!shell_pin_gate_.apply_pin_gate) {
    return;
  }
  link_flow_ = true;
  ResetSettingsFlows();
  pin_state_ = {};
  pin_state_.active = true;
  pin_state_.chooser_mode = true;
  pin_state_.title = Tr("pin.link_chooser_title").c_str();
  pin_state_.message = (phrase_flow_ ? Tr("pin.phrase_chooser_message") : Tr("pin.link_chooser_message")).c_str();
  ApplyPinGate();
  if (shell_pin_gate_.remount_pin_gate) {
    shell_pin_gate_.remount_pin_gate();
  }
}

void PinGateController::ShowUnlock() {
  if (!shell_pin_gate_.apply_pin_gate) {
    return;
  }
  link_flow_ = false;
  phrase_flow_ = false;
  ResetSettingsFlows();
  pin_state_ = {};
  pin_state_.active = true;
  pin_state_.create_mode = false;
  pin_state_.unlock_mode = true;
  pin_state_.title = Tr("pin.unlock_title").c_str();
  SetupKnownPinEntry();
  // Face / fingerprint unlock on (at-rest A014): the door opens with the sensor's key and "Enter PIN" only; after
  // that button the pad's bottom-left key / the field's link prompt again.
  const std::string biometric_kind = keypad_ports_.biometric_kind ? keypad_ports_.biometric_kind() : std::string();
  pin_state_.biometric_key = !biometric_kind.empty();
  pin_state_.biometric_kind = biometric_kind.c_str();
  pin_state_.biometric_first = pin_state_.biometric_key;
  pin_state_.message = UnlockMessage().c_str();
  ApplyPinGate();
  if (shell_pin_gate_.remount_pin_gate) {
    shell_pin_gate_.remount_pin_gate();
  }
}

void PinGateController::ShowCreate() {
  if (!shell_pin_gate_.apply_pin_gate) {
    return;
  }
  pin_state_.identity_fork_mode = false;
  pin_state_.link_paste_mode = false;
  pin_state_.chooser_mode = false;
  pin_state_.create_mode = true;
  pin_state_.error = "";
  pin_state_.title = Tr("pin.create_title").c_str();
  SetupNewPinEntry();
  pin_state_.message = NewPinMessage().c_str();
  ApplyPinGate();
  if (shell_pin_gate_.remount_pin_gate) {
    shell_pin_gate_.remount_pin_gate();
  }
}

void PinGateController::ShowLinkPaste() {
  if (!shell_pin_gate_.apply_pin_gate) {
    return;
  }
  pin_state_.identity_fork_mode = false;
  pin_state_.chooser_mode = false;
  pin_state_.create_mode = false;
  pin_state_.keypad = false;
  pin_state_.keypad_offered = false;
  pin_state_.keypad_busy = false;
  pin_state_.ask_confirm = false;
  pin_state_.link_paste_mode = true;
  pin_state_.phrase_flow = phrase_flow_;
  pin_state_.error = "";
  pin_state_.link_payload = "";
  pin_state_.saved_copy_name = "";
  saved_copy_.clear();
  pin_state_.title = (phrase_flow_ ? Tr("pin.phrase_paste_title") : Tr("pin.link_paste_title")).c_str();
  pin_state_.message = (phrase_flow_ ? Tr("pin.phrase_paste_message") : Tr("pin.link_paste_message")).c_str();
  ApplyPinGate();
  if (shell_pin_gate_.remount_pin_gate) {
    shell_pin_gate_.remount_pin_gate();
  }
}

void PinGateController::ShowVerify(const PinVerifyPurpose purpose, PinGateCheck check, std::function<void()> on_ok) {
  if (!shell_pin_gate_.apply_pin_gate) {
    return;
  }
  link_flow_ = false;
  phrase_flow_ = false;
  ResetSettingsFlows();
  verify_purpose_ = purpose;
  verify_check_ = std::move(check);
  verify_on_ok_ = std::move(on_ok);
  pin_state_ = {};
  pin_state_.active = true;
  pin_state_.verify_mode = true;
  pin_state_.title = Tr("pin.verify_title").c_str();
  switch (purpose) {
  case PinVerifyPurpose::ExportTransferCode:
    pin_state_.message = Tr("pin.verify_export_message").c_str();
    break;
  case PinVerifyPurpose::ShowRecoveryPhrase:
    pin_state_.message = Tr("pin.verify_recovery_message").c_str();
    break;
  case PinVerifyPurpose::EnableBiometric:
    pin_state_.message = Tr("pin.verify_biometric_message").c_str();
    break;
  }
  SetupKnownPinEntry();
  ApplyPinGate();
  Remount();
}

void PinGateController::ShowChange(PinGateCheck check_old, PinGateChange change,
                                   std::optional<SecretText> current_pin) {
  if (!shell_pin_gate_.apply_pin_gate) {
    return;
  }
  link_flow_ = false;
  phrase_flow_ = false;
  ResetSettingsFlows();
  change_check_old_ = std::move(check_old);
  change_submit_ = std::move(change);
  change_step_ = 0;
  pin_state_ = {};
  pin_state_.active = true;
  pin_state_.change_mode = true;
  pin_state_.title = Tr("settings.security.change_pin").c_str();
  if (current_pin) {
    // The app default PIN: straight to the new PIN.
    change_old_pin_ = std::move(*current_pin);
    change_step_ = 1;
    SetupNewPinEntry();
    pin_state_.message = NewPinMessage().c_str();
  } else {
    pin_state_.message = Tr("pin.change_old_message").c_str();
    SetupKnownPinEntry();
  }
  ApplyPinGate();
  Remount();
}

void PinGateController::OnIdentityNew() {
  if (!pin_state_.active || !pin_state_.identity_fork_mode) {
    return;
  }
  ShowChooser();
}

void PinGateController::OnIdentityLink() {
  if (!pin_state_.active || !pin_state_.identity_fork_mode) {
    return;
  }
  ShowLinkChooser();
}

void PinGateController::OnIdentityPhrase() {
  if (!pin_state_.active || !pin_state_.identity_fork_mode) {
    return;
  }
  phrase_flow_ = true;
  ShowLinkChooser();
}

void PinGateController::OnUseSavedCopy() {
  if (!pin_state_.active || !pin_state_.link_paste_mode || !phrase_flow_ || !gate_complete_.pick_saved_copy) {
    return;
  }
  PullBoundPinFields();  // keep the words typed so far
  gate_complete_.pick_saved_copy([this](std::string name, std::optional<std::string> text) {
    if (name.empty() || !pin_state_.active || !pin_state_.link_paste_mode || !phrase_flow_) {
      return;
    }
    PullBoundPinFields();
    if (!text) {
      pin_state_.error = Tr("pin.phrase_saved_copy_unreadable").c_str();
    } else {
      pin_state_.error = "";
      saved_copy_ = std::move(*text);
      pin_state_.saved_copy_name = name.c_str();
    }
    ApplyPinGate();
    DirtyPinFields();
  });
}

void PinGateController::OnForgotToggle() {
  if (!shell_pin_gate_.apply_pin_gate || !pin_state_.active || !pin_state_.unlock_mode) {
    return;
  }
  PullBoundPinFields();
  pin_state_.forgot_open = !pin_state_.forgot_open;
  ApplyPinGate();
  DirtyPinFields();
}

void PinGateController::OnSetPin() {
  if (!shell_pin_gate_.apply_pin_gate) {
    return;
  }
  if (!pin_state_.active || !pin_state_.chooser_mode) {
    return;
  }
  ShowCreate();
}

void PinGateController::OnUseDefaultPin() {
  if (!shell_pin_gate_.apply_pin_gate) {
    return;
  }
  if (!pin_state_.active || !pin_state_.chooser_mode) {
    return;
  }
  if (link_flow_) {
    pending_link_pin_ = std::string(kDefaultProfilePin);
    pending_link_default_pin_ = true;
    ShowLinkPaste();
    return;
  }
  if (!gate_complete_.complete_with_default_pin) {
    return;
  }

  PullBoundPinFields();
  pin_state_.error = "";
  ApplyPinGate();
  DirtyPinFields();
  gate_complete_.complete_with_default_pin();
}

void PinGateController::OnSubmit() {
  if (!shell_pin_gate_.apply_pin_gate) {
    return;
  }
  PullBoundPinFields();
  if (!pin_state_.active || pin_state_.chooser_mode || pin_state_.identity_fork_mode ||
      pin_state_.waiting_device_unlock || pin_state_.keypad || pin_state_.keypad_busy) {
    return;  // the pad has no Continue: a full PIN submits itself
  }

  if (pin_state_.link_paste_mode) {
    // B166/B167: an over-long paste is kept out of the rendered link_payload (see ShellHost::PinGatePaste); read the
    // full value from link_payload_full when it was hidden.
    const std::string payload =
        pin_state_.payload_hidden ? pin_state_.link_payload_full.c_str() : pin_state_.link_payload.c_str();
    if (payload.empty()) {
      pin_state_.error = (phrase_flow_ ? Tr("pin.phrase_paste_empty") : Tr("pin.link_paste_empty")).c_str();
      ApplyPinGate();
      DirtyPinFields();
      return;
    }
    pin_state_.error = "";
    ApplyPinGate();
    DirtyPinFields();
    if (phrase_flow_) {
      if (gate_complete_.complete_phrase_restore) {
        std::vector<std::string> saved_copies;
        if (!saved_copy_.empty()) {
          saved_copies.push_back(saved_copy_);
        }
        gate_complete_.complete_phrase_restore(pending_link_pin_, payload, pending_link_default_pin_, saved_copies);
      }
      return;
    }
    if (gate_complete_.complete_link_device) {
      gate_complete_.complete_link_device(pending_link_pin_, payload, pending_link_default_pin_);
    }
    return;
  }

  const std::string pin = pin_state_.pin.c_str();
  if (pin.empty()) {
    pin_state_.error = Tr("pin.error.required").c_str();
    ApplyPinGate();
    DirtyPinFields();
    return;
  }
  if (InNewPinStep()) {
    if (pin_state_.create_mode && !gate_complete_.complete_with_pin) {
      return;
    }
    SubmitNewPin(pin, pin_state_.pin_confirm.c_str());
    return;
  }
  SubmitKnownPin(pin);
}

void PinGateController::SubmitKnownPin(const std::string& pin) {
  if (pin_state_.unlock_mode) {
    if (!gate_complete_.complete_with_pin) {
      return;
    }
    pin_state_.error = "";
    pin_state_.keypad_busy = pin_state_.keypad;  // until the vault answers (Dismiss, or ShowError)
    ApplyPinGate();
    DirtyPinFields();
    gate_complete_.complete_with_pin(pin, /*create_mode=*/false);
    return;
  }
  const bool verify = pin_state_.verify_mode;
  if (!verify && !(pin_state_.change_mode && change_step_ == 0)) {
    return;
  }
  // A copy: the check may answer at once, and the gate's answer may drop the stored one.
  PinGateCheck check = verify ? verify_check_ : change_check_old_;
  if (!check) {
    return;
  }
  pin_state_.error = "";
  pin_state_.keypad_busy = true;
  ApplyPinGate();
  DirtyPinFields();
  const uint64_t request = ++request_;
  if (verify) {
    check(pin, [this, request](Roe<void> result) {
      OnCheckDone(request, std::move(result), [this]() {
        std::function<void()> on_ok = std::move(verify_on_ok_);
        Dismiss();
        if (on_ok) {
          on_ok();
        }
      });
    });
    return;
  }
  const SecretText old_pin(pin);
  check(pin, [this, request, old_pin](Roe<void> result) {
    OnCheckDone(request, std::move(result), [this, old_pin]() {
      change_old_pin_ = old_pin;
      change_step_ = 1;
      pin_state_.error = "";
      SetupNewPinEntry();
      pin_state_.message = NewPinMessage().c_str();
      ApplyPinGate();
      DirtyPinFields();
      Remount();
    });
  });
}

void PinGateController::SubmitNewPin(const std::string& pin, const std::string& confirm) {
  if (pin != confirm) {
    pin_state_.error = Tr("pin.error.mismatch").c_str();
    if (pin_state_.keypad) {
      // Only the content can differ (the repeat has the first entry's length): both entries again.
      pin_state_.pin = "";
      pin_state_.pin_confirm = "";
      new_pin_step_ = 0;
      pin_state_.message = NewPinMessage().c_str();
      RefreshKeypad();
    }
    ApplyPinGate();
    DirtyPinFields();
    return;
  }
  if (!PinDigitsOf(pin)) {
    pin_state_.error = Tr("errors.pin.too_short").c_str();
    ApplyPinGate();
    DirtyPinFields();
    return;
  }
  if (pin_state_.create_mode) {
    if (!gate_complete_.complete_with_pin) {
      return;
    }
    if (link_flow_) {
      pending_link_pin_ = pin;
      pending_link_default_pin_ = false;
      ShowLinkPaste();
      return;
    }
    pin_state_.error = "";
    pin_state_.keypad_busy = pin_state_.keypad;
    ApplyPinGate();
    DirtyPinFields();
    gate_complete_.complete_with_pin(pin, /*create_mode=*/true);
    return;
  }
  PinGateChange change = change_submit_;
  if (!change) {
    return;
  }
  pin_state_.error = "";
  pin_state_.keypad_busy = true;
  ApplyPinGate();
  DirtyPinFields();
  const uint64_t request = ++request_;
  change(change_old_pin_, pin, [this, request](Roe<void> result) {
    OnCheckDone(request, std::move(result), [this]() { Dismiss(); });
  });
}

void PinGateController::OnCheckDone(const uint64_t request, Roe<void> result, std::function<void()> then) {
  if (request != request_ || !pin_state_.active) {
    return;  // cancelled, or another flow took the gate, while it was checked
  }
  PullBoundPinFields();
  pin_state_.keypad_busy = false;
  if (!result) {
    pin_state_.error =
        (ProfileUnlockGate::IsWrongPinError(result.error()) ? Tr("pin.error.wrong")
                                                             : UserFeedback::UserMessage(result.error()))
            .c_str();
    pin_state_.pin = "";
    pin_state_.pin_confirm = "";
    if (InNewPinStep()) {
      new_pin_step_ = 0;
      pin_state_.message = NewPinMessage().c_str();
    }
    RefreshKeypad();
    ApplyPinGate();
    DirtyPinFields();
    return;
  }
  then();
}

void PinGateController::OnKey(const int digit) {
  if (!pin_state_.active || !pin_state_.keypad || pin_state_.keypad_busy || pin_state_.waiting_device_unlock ||
      digit < 0 || digit > 9) {
    return;
  }
  const char key = static_cast<char>('0' + digit);
  std::string pin = pin_state_.pin.c_str();
  if (InNewPinStep()) {
    if (new_pin_step_ == 0) {
      if (pin.size() >= 8) {
        return;
      }
      pin += key;
      pin_state_.pin = pin.c_str();
      pin_state_.error = "";
      RefreshKeypad();
      ApplyPinGate();
      DirtyPinFields();
      return;
    }
    std::string confirm = pin_state_.pin_confirm.c_str();
    if (confirm.size() >= pin.size()) {
      return;
    }
    confirm += key;
    pin_state_.pin_confirm = confirm.c_str();
    pin_state_.error = "";
    RefreshKeypad();
    ApplyPinGate();
    DirtyPinFields();
    if (confirm.size() == pin.size()) {
      SubmitNewPin(pin, confirm);
    }
    return;
  }
  if (static_cast<int>(pin.size()) >= pin_state_.keypad_slots) {
    return;
  }
  pin += key;
  pin_state_.pin = pin.c_str();
  pin_state_.error = "";
  RefreshKeypad();
  ApplyPinGate();
  DirtyPinFields();
  if (static_cast<int>(pin.size()) == pin_state_.keypad_slots) {
    SubmitKnownPin(pin);
  }
}

void PinGateController::OnBackspace() {
  if (!pin_state_.active || !pin_state_.keypad || pin_state_.keypad_busy || pin_state_.waiting_device_unlock) {
    return;
  }
  // The repeat's backspace stays in the repeat (an empty repeat does not go back to the first entry).
  ui::String& field = InNewPinStep() && new_pin_step_ == 1 ? pin_state_.pin_confirm : pin_state_.pin;
  std::string typed = field.c_str();
  if (typed.empty()) {
    return;
  }
  typed.pop_back();
  field = typed.c_str();
  RefreshKeypad();
  ApplyPinGate();
  DirtyPinFields();
}

void PinGateController::OnKeypadOk() {
  if (!pin_state_.active || !pin_state_.keypad || pin_state_.keypad_busy || !InNewPinStep() || new_pin_step_ != 0 ||
      std::string(pin_state_.pin.c_str()).size() < 4) {
    return;
  }
  new_pin_step_ = 1;
  pin_state_.pin_confirm = "";
  pin_state_.error = "";
  pin_state_.message = NewPinMessage().c_str();
  RefreshKeypad();
  ApplyPinGate();
  DirtyPinFields();
}

void PinGateController::OnUseText() {
  if (!pin_state_.active || !pin_state_.keypad || pin_state_.keypad_busy || pin_state_.waiting_device_unlock) {
    return;
  }
  pin_state_.keypad = false;
  pin_state_.pin = "";
  pin_state_.pin_confirm = "";
  pin_state_.error = "";
  new_pin_step_ = 0;
  pin_state_.ask_confirm = InNewPinStep();
  if (InNewPinStep()) {
    pin_state_.message = NewPinMessage().c_str();
  }
  RefreshKeypad();
  ApplyPinGate();
  Remount();  // the field instead of the pad
}

void PinGateController::OnUseKeypad() {
  if (!pin_state_.active || pin_state_.keypad || !pin_state_.keypad_offered || pin_state_.keypad_busy ||
      pin_state_.waiting_device_unlock) {
    return;
  }
  PullBoundPinFields();
  pin_state_.keypad = true;
  pin_state_.pin = "";
  pin_state_.pin_confirm = "";
  pin_state_.error = "";
  pin_state_.ask_confirm = false;
  new_pin_step_ = 0;
  if (InNewPinStep()) {
    pin_state_.message = NewPinMessage().c_str();
  }
  RefreshKeypad();
  ApplyPinGate();
  Remount();  // the pad instead of the field
}

void PinGateController::OnCancel() {
  if (!pin_state_.active) {
    return;
  }
  if (pin_state_.verify_mode || pin_state_.change_mode) {
    Dismiss();  // back to settings; a check still running is void
    return;
  }
  const bool cancellable = pin_state_.create_mode || pin_state_.chooser_mode || pin_state_.identity_fork_mode ||
                           pin_state_.link_paste_mode;
  if (!cancellable || !gate_complete_.cancel) {
    return;
  }
  gate_complete_.cancel();
}

} // namespace pbr
