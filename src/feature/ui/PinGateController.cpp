#include <stdexcept>
#include "feature/ui/PinGateController.h"

#include "base/crypto/PinDefaults.h"
#include "base/crypto/ProfileSecretsService.h"
#include "base/data/SessionStore.h"
#include "base/i18n/LocalizationService.h"
#include "common/StartupTiming.h"
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
  return PinGateController::Instance().Hub().EnsureMessagingReady();
}

void SetUnlockInProgressUi(const bool in_progress) {
  ShellHost::Instance().State().unlock_in_progress = in_progress;
  if (in_progress) {
    ShellHost::Instance().SetActivity(true, "Preparing...");
  } else {
    ShellHost::Instance().SetActivity(false);
  }
  DataModelHost::Instance().Dirty("window", "unlock_in_progress");
  ShellHost::Instance().DirtyWindow();
}

} // namespace

PinGateController& PinGateController::Instance() {
  static PinGateController controller;
  return controller;
}
void PinGateController::BindMessaging(MessagingHub& messaging) {
  messaging_ = &messaging;
}

MessagingHub& PinGateController::Hub() {
  if (!messaging_) {
    throw std::runtime_error("PinGateController messaging not bound");
  }
  return *messaging_;
}

const MessagingHub& PinGateController::Hub() const {
  if (!messaging_) {
    throw std::runtime_error("PinGateController messaging not bound");
  }
  return *messaging_;
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
  unlock_in_progress_ = false;
  SetUnlockInProgressUi(false);
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
  StartupPhase phase("PinGate::TrySilentDefaultUnlock");
  if (!SessionStore::Instance().IsInitialized()) {
    return false;
  }
  if (!SessionStore::Instance().Snapshot().profile_prefs.pin_is_default) {
    return false;
  }
  if (Instance().Hub().IsMessagingReady()) {
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
  return static_cast<bool>(Instance().Hub().EnsureMessagingReady());
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
  if (!Instance().Hub().IsInitialized()) {
    if (done) {
      done(false);
    }
    return;
  }
  if (Instance().Hub().IsMessagingReady()) {
    if (done) {
      done(true);
    }
    return;
  }
  // Deferred silent unlock still running — queue until it finishes.
  if (unlock_in_progress_) {
    if (done) {
      pending_.push_back(std::move(done));
    }
    return;
  }
  if (ProfileSecretsService::Instance().IsUnlocked()) {
    if (auto ready = Instance().Hub().EnsureMessagingReady(); ready) {
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
  if (SessionStore::Instance().IsInitialized() &&
      SessionStore::Instance().Snapshot().profile_prefs.pin_is_default) {
    if (done) {
      pending_.push_back(std::move(done));
    }
    BeginDeferredUnlockAfterFirstPresent();
    return;
  }
  ShowGate(false, std::move(done));
}

void PinGateController::BeginDeferredUnlockAfterFirstPresent() {
  if (deferred_unlock_started_) {
    return;
  }
  deferred_unlock_started_ = true;

  if (!Instance().Hub().IsInitialized()) {
    DrainQueue(false);
    return;
  }
  if (Instance().Hub().IsMessagingReady()) {
    DrainQueue(true);
    return;
  }
  if (!ProfileSecretsService::Instance().NeedsUnlock()) {
    if (ProfileSecretsService::Instance().IsUnlocked()) {
      const bool ready = static_cast<bool>(Instance().Hub().EnsureMessagingReady());
      DrainQueue(ready);
    } else {
      // No vault yet — leave locked until a feature calls EnsureUnlocked (chooser).
      DrainQueue(false);
    }
    return;
  }

  if (SessionStore::Instance().IsInitialized() &&
      SessionStore::Instance().Snapshot().profile_prefs.pin_is_default) {
    unlock_in_progress_ = true;
    SetUnlockInProgressUi(true);
    StartupMark("deferred_silent_unlock_begin");
    const bool ok = TrySilentDefaultUnlock();
    unlock_in_progress_ = false;
    SetUnlockInProgressUi(false);
    StartupMark(ok ? "deferred_silent_unlock_ok" : "deferred_silent_unlock_fail");
    if (ok) {
      DrainQueue(true);
      return;
    }
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
