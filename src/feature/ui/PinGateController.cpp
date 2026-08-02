#include "feature/ui/PinGateController.h"

#include "base/i18n/LocalizationService.h"
#include "feature/ui/DataModelHost.h"
#include "feature/ui/ShellHost.h"

namespace pbr {

void PinGateController::BindGate(ProfileUnlockGate& gate) {
  gate_ = &gate;
}

void PinGateController::DirtyPinFields() {
  DataModelHost::Instance().Dirty("window", "pin_gate_active");
  DataModelHost::Instance().Dirty("window", "pin_gate_chooser_mode");
  DataModelHost::Instance().Dirty("window", "pin_gate_create_mode");
  DataModelHost::Instance().Dirty("window", "pin_gate_title");
  DataModelHost::Instance().Dirty("window", "pin_gate_message");
  DataModelHost::Instance().Dirty("window", "pin_gate_error");
  DataModelHost::Instance().Dirty("window", "pin_gate_pin");
  DataModelHost::Instance().Dirty("window", "pin_gate_pin_confirm");
}

void PinGateController::SetUnlockInProgressUi(const bool in_progress) {
  ShellHost::Instance().State().unlock_in_progress = in_progress;
  if (in_progress) {
    ShellHost::Instance().SetActivity(true, "Preparing...");
  } else {
    ShellHost::Instance().SetActivity(false);
  }
  DataModelHost::Instance().Dirty("window", "unlock_in_progress");
  ShellHost::Instance().DirtyWindow();
}

void PinGateController::ShowError(const std::string& message) {
  PinGateState& gate = ShellHost::Instance().State().pin_gate;
  if (!gate.active) {
    ShowUnlock();
  }
  gate.error = message.c_str();
  DirtyPinFields();
  ShellHost::Instance().DirtyWindow();
}

void PinGateController::Dismiss() {
  ShellHost::Instance().State().pin_gate = {};
  ShellHost::Instance().RequestSyncLayout();
  ShellHost::Instance().DirtyWindow();
}

void PinGateController::ShowChooser() {
  PinGateState& gate = ShellHost::Instance().State().pin_gate;
  gate = {};
  gate.active = true;
  gate.chooser_mode = true;
  gate.title = Tr("pin.chooser_title").c_str();
  gate.message = Tr("pin.chooser_message").c_str();
  ShellHost::Instance().RequestSyncLayout();
  DirtyPinFields();
  ShellHost::Instance().DirtyWindow();
}

void PinGateController::ShowUnlock() {
  PinGateState& gate = ShellHost::Instance().State().pin_gate;
  gate = {};
  gate.active = true;
  gate.create_mode = false;
  gate.title = Tr("pin.unlock_title").c_str();
  gate.message = Tr("pin.unlock_message").c_str();
  ShellHost::Instance().RequestSyncLayout();
  DirtyPinFields();
  ShellHost::Instance().DirtyWindow();
}

void PinGateController::ShowCreate() {
  PinGateState& gate = ShellHost::Instance().State().pin_gate;
  gate.chooser_mode = false;
  gate.create_mode = true;
  gate.error = "";
  gate.pin = "";
  gate.pin_confirm = "";
  gate.title = Tr("pin.create_title").c_str();
  gate.message = Tr("pin.create_message").c_str();
  DirtyPinFields();
  ShellHost::Instance().RequestSyncLayout();
  ShellHost::Instance().DirtyWindow();
}

void PinGateController::OnSetPin() {
  PinGateState& gate = ShellHost::Instance().State().pin_gate;
  if (!gate.active || !gate.chooser_mode) {
    return;
  }
  ShowCreate();
}

void PinGateController::OnUseDefaultPin() {
  PinGateState& gate = ShellHost::Instance().State().pin_gate;
  if (!gate.active || !gate.chooser_mode || !gate_) {
    return;
  }

  gate.error = "";
  DirtyPinFields();
  gate_->CompleteWithDefaultPin();
}

void PinGateController::OnSubmit() {
  PinGateState& gate = ShellHost::Instance().State().pin_gate;
  if (!gate.active || gate.chooser_mode || !gate_) {
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
  gate_->CompleteWithPin(pin, gate.create_mode);
}

void PinGateController::OnCancel() {
  PinGateState& gate = ShellHost::Instance().State().pin_gate;
  if (!gate.active || (!gate.create_mode && !gate.chooser_mode) || !gate_) {
    return;
  }
  gate_->Cancel();
}

} // namespace pbr
