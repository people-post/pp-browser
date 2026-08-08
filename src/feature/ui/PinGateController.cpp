#include "feature/ui/PinGateController.h"

#include "base/i18n/LocalizationService.h"

namespace pbr {

void PinGateController::BindGateComplete(UnlockGateCompletePorts ports) {
  gate_complete_ = std::move(ports);
}

void PinGateController::BindShellPinGate(ShellPinGatePorts ports) {
  shell_pin_gate_ = std::move(ports);
}

void PinGateController::DirtyPinFields() {
  if (shell_pin_gate_.dirty_pin_gate) {
    shell_pin_gate_.dirty_pin_gate();
  }
}

void PinGateController::SetUnlockInProgressUi(const bool in_progress) {
  if (shell_pin_gate_.unlock_in_progress) {
    shell_pin_gate_.unlock_in_progress() = in_progress;
  }
  if (in_progress) {
    if (shell_pin_gate_.set_activity) {
      shell_pin_gate_.set_activity(true, "Preparing...");
    }
  } else if (shell_pin_gate_.set_activity) {
    shell_pin_gate_.set_activity(false, {});
  }
  DirtyPinFields();
}

void PinGateController::ShowError(const std::string& message) {
  if (!shell_pin_gate_.pin_gate) {
    return;
  }
  PinGateState& gate = shell_pin_gate_.pin_gate();
  if (!gate.active) {
    ShowUnlock();
  }
  gate.error = message.c_str();
  DirtyPinFields();
}

void PinGateController::Dismiss() {
  if (shell_pin_gate_.pin_gate) {
    shell_pin_gate_.pin_gate() = {};
  }
  if (shell_pin_gate_.request_sync_layout) {
    shell_pin_gate_.request_sync_layout(false, nullptr);
  }
}

void PinGateController::ShowChooser() {
  if (!shell_pin_gate_.pin_gate) {
    return;
  }
  PinGateState& gate = shell_pin_gate_.pin_gate();
  gate = {};
  gate.active = true;
  gate.chooser_mode = true;
  gate.title = Tr("pin.chooser_title").c_str();
  gate.message = Tr("pin.chooser_message").c_str();
  if (shell_pin_gate_.request_sync_layout) {
    shell_pin_gate_.request_sync_layout(false, nullptr);
  }
}

void PinGateController::ShowUnlock() {
  if (!shell_pin_gate_.pin_gate) {
    return;
  }
  PinGateState& gate = shell_pin_gate_.pin_gate();
  gate = {};
  gate.active = true;
  gate.create_mode = false;
  gate.title = Tr("pin.unlock_title").c_str();
  gate.message = Tr("pin.unlock_message").c_str();
  if (shell_pin_gate_.request_sync_layout) {
    shell_pin_gate_.request_sync_layout(false, nullptr);
  }
}

void PinGateController::ShowCreate() {
  if (!shell_pin_gate_.pin_gate) {
    return;
  }
  PinGateState& gate = shell_pin_gate_.pin_gate();
  gate.chooser_mode = false;
  gate.create_mode = true;
  gate.error = "";
  gate.pin = "";
  gate.pin_confirm = "";
  gate.title = Tr("pin.create_title").c_str();
  gate.message = Tr("pin.create_message").c_str();
  if (shell_pin_gate_.request_sync_layout) {
    shell_pin_gate_.request_sync_layout(false, nullptr);
  }
}

void PinGateController::OnSetPin() {
  if (!shell_pin_gate_.pin_gate) {
    return;
  }
  PinGateState& gate = shell_pin_gate_.pin_gate();
  if (!gate.active || !gate.chooser_mode) {
    return;
  }
  ShowCreate();
}

void PinGateController::OnUseDefaultPin() {
  if (!shell_pin_gate_.pin_gate) {
    return;
  }
  PinGateState& gate = shell_pin_gate_.pin_gate();
  if (!gate.active || !gate.chooser_mode || !gate_complete_.complete_with_default_pin) {
    return;
  }

  gate.error = "";
  DirtyPinFields();
  gate_complete_.complete_with_default_pin();
}

void PinGateController::OnSubmit() {
  if (!shell_pin_gate_.pin_gate) {
    return;
  }
  PinGateState& gate = shell_pin_gate_.pin_gate();
  if (!gate.active || gate.chooser_mode || !gate_complete_.complete_with_pin) {
    return;
  }

  const std::string pin = gate.pin.c_str();
  if (pin.empty()) {
    gate.error = "PIN is required";
    DirtyPinFields();
    return;
  }
  if (gate.create_mode) {
    const std::string confirm = gate.pin_confirm.c_str();
    if (pin != confirm) {
      gate.error = "PINs do not match";
      DirtyPinFields();
      return;
    }
    if (pin.size() < 4) {
      gate.error = "Use at least 4 characters";
      DirtyPinFields();
      return;
    }
  }

  gate.error = "";
  DirtyPinFields();
  gate_complete_.complete_with_pin(pin, gate.create_mode);
}

void PinGateController::OnCancel() {
  if (!shell_pin_gate_.pin_gate) {
    return;
  }
  PinGateState& gate = shell_pin_gate_.pin_gate();
  if (!gate.active || (!gate.create_mode && !gate.chooser_mode) || !gate_complete_.cancel) {
    return;
  }
  gate_complete_.cancel();
}

} // namespace pbr
