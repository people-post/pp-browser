#include "feature/ui/PinGateController.h"

#include "feature/messaging/MessagingHub.h"
#include "feature/ui/DataModelHost.h"
#include "feature/ui/ShellHost.h"

namespace pbr {

PinGateController& PinGateController::Instance() {
  static PinGateController controller;
  return controller;
}

void PinGateController::DrainQueue(const bool unlocked) {
  std::vector<std::function<void(bool)>> queued;
  queued.swap(pending_);
  showing_ = false;
  for (auto& cb : queued) {
    if (cb) {
      cb(unlocked);
    }
  }
}

void PinGateController::Finish(const bool unlocked) {
  ShellHost::Instance().State().pin_gate = {};
  ShellHost::Instance().RequestSyncLayout();
  ShellHost::Instance().DirtyWindow();
  DrainQueue(unlocked);
}

void PinGateController::DirtyPinFields() {
  DataModelHost::Instance().Dirty("window", "pin_gate_active");
  DataModelHost::Instance().Dirty("window", "pin_gate_create_mode");
  DataModelHost::Instance().Dirty("window", "pin_gate_title");
  DataModelHost::Instance().Dirty("window", "pin_gate_message");
  DataModelHost::Instance().Dirty("window", "pin_gate_error");
  DataModelHost::Instance().Dirty("window", "pin_gate_pin");
  DataModelHost::Instance().Dirty("window", "pin_gate_pin_confirm");
}

void PinGateController::ShowGate(const bool create_mode, std::function<void(bool)> done) {
  if (done) {
    pending_.push_back(std::move(done));
  }
  if (showing_) {
    return;
  }
  showing_ = true;

  PinGateState& gate = ShellHost::Instance().State().pin_gate;
  gate = {};
  gate.active = true;
  gate.create_mode = create_mode;
  if (create_mode) {
    gate.title = "Create a PIN";
    gate.message = "Protect your identity and chat keys with a PIN. You'll need it next time you open this profile.";
  } else {
    gate.title = "Unlock profile";
    gate.message = "Enter your PIN to unlock identity and encrypted keys.";
  }
  ShellHost::Instance().RequestSyncLayout();
  DirtyPinFields();
  ShellHost::Instance().DirtyWindow();
}

void PinGateController::EnsureUnlocked(std::function<void(bool unlocked)> done) {
  if (!MessagingHub::Instance().IsInitialized()) {
    if (done) {
      done(false);
    }
    return;
  }
  if (MessagingHub::Instance().AreSecretsReady()) {
    if (done) {
      done(true);
    }
    return;
  }
  ShowGate(!MessagingHub::Instance().HasVault(), std::move(done));
}

void PinGateController::PromptUnlockIfVaultExists() {
  if (!MessagingHub::Instance().IsInitialized()) {
    return;
  }
  if (MessagingHub::Instance().AreSecretsReady()) {
    return;
  }
  if (!MessagingHub::Instance().NeedsVaultUnlock()) {
    return;
  }
  ShowGate(false, nullptr);
}

void PinGateController::OnSubmit() {
  PinGateState& gate = ShellHost::Instance().State().pin_gate;
  if (!gate.active) {
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

  auto unlocked = MessagingHub::Instance().EnsureSecretsUnlocked(pin);
  if (!unlocked) {
    gate.error = unlocked.error().message.c_str();
    gate.pin = "";
    gate.pin_confirm = "";
    DirtyPinFields();
    return;
  }

  Finish(true);
}

void PinGateController::OnCancel() {
  PinGateState& gate = ShellHost::Instance().State().pin_gate;
  if (!gate.active || !gate.create_mode) {
    return;
  }
  Finish(false);
}

} // namespace pbr
