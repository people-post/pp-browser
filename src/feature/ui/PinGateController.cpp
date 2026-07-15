#include "feature/ui/PinGateController.h"

#include "base/crypto/PinDefaults.h"
#include "base/crypto/ProfileSecretsService.h"
#include "base/data/SessionStore.h"
#include "base/i18n/LocalizationService.h"
#include "feature/messaging/MessagingHub.h"
#include "feature/ui/DataModelHost.h"
#include "feature/ui/ShellFeedback.h"
#include "feature/ui/ShellHost.h"

namespace pbr {

namespace {

Roe<void> UnlockProfileAndMessaging(const std::string& pin) {
  if (auto unlocked = ProfileSecretsService::Instance().Unlock(pin); !unlocked) {
    return unlocked.error();
  }
  return MessagingHub::Instance().EnsureMessagingReady();
}

} // namespace

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
  DataModelHost::Instance().Dirty("window", "pin_gate_chooser_mode");
  DataModelHost::Instance().Dirty("window", "pin_gate_create_mode");
  DataModelHost::Instance().Dirty("window", "pin_gate_title");
  DataModelHost::Instance().Dirty("window", "pin_gate_message");
  DataModelHost::Instance().Dirty("window", "pin_gate_error");
  DataModelHost::Instance().Dirty("window", "pin_gate_pin");
  DataModelHost::Instance().Dirty("window", "pin_gate_pin_confirm");
}

void PinGateController::SetPinIsDefault(const bool is_default) {
  if (!SessionStore::Instance().IsInitialized()) {
    return;
  }
  ProfilePreferences prefs = SessionStore::Instance().Snapshot().profile_prefs;
  prefs.pin_is_default = is_default;
  (void)SessionStore::Instance().SaveProfilePrefs(prefs);
}

bool PinGateController::TrySilentDefaultUnlock() {
  if (!SessionStore::Instance().IsInitialized()) {
    return false;
  }
  if (!SessionStore::Instance().Snapshot().profile_prefs.pin_is_default) {
    return false;
  }
  if (MessagingHub::Instance().IsMessagingReady()) {
    return true;
  }
  if (!ProfileSecretsService::Instance().HasVault()) {
    return false;
  }
  if (!ProfileSecretsService::Instance().IsUnlocked()) {
    if (!ProfileSecretsService::Instance().Unlock(kDefaultProfilePin)) {
      return false;
    }
  }
  return static_cast<bool>(MessagingHub::Instance().EnsureMessagingReady());
}

void PinGateController::ShowChooser(std::function<void(bool)> done) {
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
  gate.chooser_mode = true;
  gate.title = Tr("pin.chooser_title").c_str();
  gate.message = Tr("pin.chooser_message").c_str();
  ShellHost::Instance().RequestSyncLayout();
  DirtyPinFields();
  ShellHost::Instance().DirtyWindow();
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
    gate.title = Tr("pin.create_title").c_str();
    gate.message = Tr("pin.create_message").c_str();
  } else {
    gate.title = Tr("pin.unlock_title").c_str();
    gate.message = Tr("pin.unlock_message").c_str();
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
  if (MessagingHub::Instance().IsMessagingReady()) {
    if (done) {
      done(true);
    }
    return;
  }
  if (ProfileSecretsService::Instance().IsUnlocked()) {
    if (auto ready = MessagingHub::Instance().EnsureMessagingReady(); ready) {
      if (done) {
        done(true);
      }
    } else if (done) {
      done(false);
    }
    return;
  }
  if (!ProfileSecretsService::Instance().HasVault()) {
    ShowChooser(std::move(done));
    return;
  }
  if (TrySilentDefaultUnlock()) {
    if (done) {
      done(true);
    }
    return;
  }
  ShowGate(false, std::move(done));
}

void PinGateController::PromptUnlockIfVaultExists() {
  if (!MessagingHub::Instance().IsInitialized()) {
    return;
  }
  if (MessagingHub::Instance().IsMessagingReady()) {
    return;
  }
  if (!ProfileSecretsService::Instance().NeedsUnlock()) {
    return;
  }
  if (TrySilentDefaultUnlock()) {
    return;
  }
  ShowGate(false, nullptr);
}

void PinGateController::OnSetPin() {
  PinGateState& gate = ShellHost::Instance().State().pin_gate;
  if (!gate.active || !gate.chooser_mode) {
    return;
  }
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

void PinGateController::OnUseDefaultPin() {
  PinGateState& gate = ShellHost::Instance().State().pin_gate;
  if (!gate.active || !gate.chooser_mode) {
    return;
  }

  auto unlocked = UnlockProfileAndMessaging(kDefaultProfilePin);
  if (!unlocked) {
    gate.error = unlocked.error().message.c_str();
    DirtyPinFields();
    return;
  }

  SetPinIsDefault(true);
  Finish(true);
  ShellFeedback::ShowToast(ShellHost::Instance().State(),
                           "Using the app default. Change anytime in Me → Security.");
  ShellHost::Instance().DirtyWindow();
}

void PinGateController::OnSubmit() {
  PinGateState& gate = ShellHost::Instance().State().pin_gate;
  if (!gate.active || gate.chooser_mode) {
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

  auto unlocked = UnlockProfileAndMessaging(pin);
  if (!unlocked) {
    gate.error = unlocked.error().message.c_str();
    gate.pin = "";
    gate.pin_confirm = "";
    DirtyPinFields();
    return;
  }

  if (gate.create_mode) {
    SetPinIsDefault(false);
  }

  Finish(true);
}

void PinGateController::OnCancel() {
  PinGateState& gate = ShellHost::Instance().State().pin_gate;
  if (!gate.active || (!gate.create_mode && !gate.chooser_mode)) {
    return;
  }
  Finish(false);
}

} // namespace pbr
