#include "gui/PinGateController.h"

#include "foundation/crypto/PinDefaults.h"
#include "foundation/i18n/LocalizationService.h"

namespace pbr {

void PinGateController::BindGateComplete(UnlockGateCompletePorts ports) {
  gate_complete_ = std::move(ports);
}

void PinGateController::BindShellPinGate(ShellPinGatePorts ports) {
  shell_pin_gate_ = std::move(ports);
}

void PinGateController::PullBoundPinFields() {
  if (!shell_pin_gate_.pin_gate_snapshot) {
    return;
  }
  const PinGateState snap = shell_pin_gate_.pin_gate_snapshot();
  pin_state_.pin = snap.pin;
  pin_state_.pin_confirm = snap.pin_confirm;
  pin_state_.link_payload = snap.link_payload;
}

void PinGateController::ApplyPinGate() {
  if (shell_pin_gate_.apply_pin_gate) {
    shell_pin_gate_.apply_pin_gate(pin_state_);
  }
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

void PinGateController::ShowError(const std::string& message) {
  if (!shell_pin_gate_.apply_pin_gate) {
    return;
  }
  if (!pin_state_.active) {
    ShowUnlock();
  }
  PullBoundPinFields();
  pin_state_.error = message.c_str();
  ApplyPinGate();
  DirtyPinFields();
}

void PinGateController::Dismiss() {
  pin_state_ = {};
  link_flow_ = false;
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
  pending_link_default_pin_ = false;
  pending_link_pin_.clear();
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
  pin_state_ = {};
  pin_state_.active = true;
  pin_state_.chooser_mode = true;
  pin_state_.title = Tr("pin.link_chooser_title").c_str();
  pin_state_.message = Tr("pin.link_chooser_message").c_str();
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
  pin_state_ = {};
  pin_state_.active = true;
  pin_state_.create_mode = false;
  pin_state_.title = Tr("pin.unlock_title").c_str();
  pin_state_.message = Tr("pin.unlock_message").c_str();
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
  pin_state_.pin = "";
  pin_state_.pin_confirm = "";
  pin_state_.title = Tr("pin.create_title").c_str();
  pin_state_.message = link_flow_ ? Tr("pin.link_create_message").c_str() : Tr("pin.create_message").c_str();
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
  pin_state_.link_paste_mode = true;
  pin_state_.error = "";
  pin_state_.link_payload = "";
  pin_state_.title = Tr("pin.link_paste_title").c_str();
  pin_state_.message = Tr("pin.link_paste_message").c_str();
  ApplyPinGate();
  if (shell_pin_gate_.remount_pin_gate) {
    shell_pin_gate_.remount_pin_gate();
  }
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
  if (!pin_state_.active || pin_state_.chooser_mode || pin_state_.identity_fork_mode) {
    return;
  }

  if (pin_state_.link_paste_mode) {
    if (!gate_complete_.complete_link_device) {
      return;
    }
    const std::string payload = pin_state_.link_payload.c_str();
    if (payload.empty()) {
      pin_state_.error = Tr("pin.link_paste_empty").c_str();
      ApplyPinGate();
      DirtyPinFields();
      return;
    }
    pin_state_.error = "";
    ApplyPinGate();
    DirtyPinFields();
    gate_complete_.complete_link_device(pending_link_pin_, payload, pending_link_default_pin_);
    return;
  }

  if (!gate_complete_.complete_with_pin) {
    return;
  }

  const std::string pin = pin_state_.pin.c_str();
  if (pin.empty()) {
    pin_state_.error = "PIN is required";
    ApplyPinGate();
    DirtyPinFields();
    return;
  }
  if (pin_state_.create_mode) {
    const std::string confirm = pin_state_.pin_confirm.c_str();
    if (pin != confirm) {
      pin_state_.error = "PINs do not match";
      ApplyPinGate();
      DirtyPinFields();
      return;
    }
    if (pin.size() < 4) {
      pin_state_.error = "Use at least 4 characters";
      ApplyPinGate();
      DirtyPinFields();
      return;
    }
    if (link_flow_) {
      pending_link_pin_ = pin;
      pending_link_default_pin_ = false;
      ShowLinkPaste();
      return;
    }
  }

  pin_state_.error = "";
  ApplyPinGate();
  DirtyPinFields();
  gate_complete_.complete_with_pin(pin, pin_state_.create_mode);
}

void PinGateController::OnCancel() {
  if (!pin_state_.active) {
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
